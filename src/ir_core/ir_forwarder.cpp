#include "ir_forwarder.h"
#include "utils/precise_timer.h"
#include "utils/timing_scratch.h"
#include "ir_core/ir_learner.h"
#include "utils/hex_utils.h"
#include "utils/memory_monitor.h"
#include "utils/logger.h"
#include "utils/net_watchdog.h"
#include "unified_signal.h"
#include <ArduinoJson.h>

IRForwarder::IRForwarder(IRMapper& mapper, IRTransmitter& transmitter, MessageQueue& queue, Logger& logger)
    : mapper(mapper), transmitter(transmitter), queue(queue), logger(logger), forwarding(false), enabled(true),
      delayState(FORWARD_DELAY_IDLE), pendingTaskValid(false),
      pendingTimingRuleId(-1), pendingTimingRecordIndex(0), pendingTimingRecordSeq(0),
      decodeMs(0), sendStartMs(0),
      lastMatchedRuleId(-1), lastMatchedMs(0), lastRepeatForwardMs(0),
      forwardRecordIndex(0), forwardRecordCount(0), nextRecordSeq(1) {
}

bool IRForwarder::init() {
    // 初始化时序生成器
    if (!generator.init()) {
        return false;
    }
    clearForwardRecords();
    return true;
}

// ── 转发记录 ────────────────────────────────────────────────────────

uint8_t IRForwarder::addForwardRecord(const UnifiedSignal& a, int16_t ruleId, uint8_t bCount, uint8_t result) {
    uint8_t idx = forwardRecordIndex;
    ForwardRecord& rec = forwardRecords[idx];
    rec.timestamp  = millis();
    rec.seq        = nextRecordSeq++;
    rec.aCode      = a.useTiming ? 0 : a.data;
    rec.durationMs = 0;
    rec.latencyMs  = 0;   // 由 markSendStart() 在发射前打点，失败路径保持 0
    rec.ruleId     = ruleId;
    rec.aProtocol  = a.useTiming ? 255 : (uint8_t)(a.protocol & 0xFF);
    rec.bCount     = bCount;
    rec.result     = result;

    forwardRecordIndex = (forwardRecordIndex + 1) % MAX_FORWARD_RECORDS;
    if (forwardRecordCount < MAX_FORWARD_RECORDS) {
        forwardRecordCount++;
    }
    return idx;
}

void IRForwarder::markSendStart(uint8_t recordIndex, uint16_t recordSeq) {
    sendStartMs = millis();
    if (recordIndex >= MAX_FORWARD_RECORDS) return;
    ForwardRecord& rec = forwardRecords[recordIndex];
    if (rec.seq != recordSeq) return;   // 已被后来的记录覆盖，别把时间写进别人那条
    rec.latencyMs = (uint16_t)(sendStartMs - rec.timestamp);
}

void IRForwarder::finishForwardRecord(uint8_t recordIndex, uint16_t recordSeq,
                                      uint8_t result, uint16_t durationMs) {
    if (recordIndex >= MAX_FORWARD_RECORDS) return;
    ForwardRecord& rec = forwardRecords[recordIndex];
    // 身份号对不上 = 这一格已经被后来的记录占用；此时回填会把结果写到别人的记录上，
    // 比不回填更糟（排障时看到的是错位的因果）。宁可让那条记录停在 result 占位值。
    if (rec.seq != recordSeq) return;
    rec.result     = result;
    rec.durationMs = durationMs;
}

bool IRForwarder::forwardRepeat(const UnifiedSignal& aSignal) {
    if (lastMatchedRuleId < 0) return false;
    if ((millis() - lastMatchedMs) >= TX_HOLD_REPEAT_WINDOW_MS) return false;   // 早松手了
    // 刚续发过一次就别再发：多半是自己那次发射的回声被自己的接收头看到了
    // （回声在发射后几毫秒就到，而遥控器的下一次重复在 ~108ms 后）。
    if (lastRepeatForwardMs != 0 &&
        (millis() - lastRepeatForwardMs) < TX_REPEAT_MIN_FORWARD_INTERVAL_MS) {
        return false;
    }

    IR_MapRule* rule = mapper.findRuleById((uint8_t)lastMatchedRuleId);
    if (rule == nullptr) return false;                    // 这期间规则被删了
    if (rule->bSequence.signalCount != 1) return false;   // 多信号序列见下
    const UnifiedSignal& b = rule->bSequence.signals[0];
    if (b.useTiming || !b.isProtocol() || b.protocol != 1) return false;

    // ⚠️ 只对"单条 NEC 的 B"续发。多信号序列（如规则 134 电源 = 整帧 + 2×重复码）
    //    续发等于把整条序列再放一遍 —— 对开关机这类命令是错的，宁可不做。
    // 续发用 sendNECRepeat()（裸重复码 9000/2250/560），与"遥控器→被控设备"的原生
    // 序列一致（先整帧、后重复码），而且只占 11ms ⇒ 聋期 = 11 + 静默 20 = **31ms**，
    // 远短于遥控器的重复周期（~108ms），不会把下一次重复挡掉。
    // ⚠️ 这条路径**不走** FORWARD_DELAY_BEFORE_SEND_MS 那 20ms 的 AB 隔离，与另外两条
    //    （延时槽 / 队列）不同。理由：AB 隔离是为了让 A 侧信号彻底结束、接收头不在
    //    采集中，而这里能走到解码完成，本身就已经隔了 IRrecv 的 15ms 静默超时 ⇒
    //    隔离条件已满足。实测确认 `latencyMs=0`、`durationMs=12`。
    //    再等 20ms 只会让被控设备晚 20ms 续上，没有收益。
    uint8_t idx = addForwardRecord(aSignal, (int16_t)rule->id, 1, FORWARD_RESULT_SENT);
    uint16_t seq = forwardRecords[idx].seq;

    markSendStart(idx, seq);
    unsigned long t0 = millis();
    forwarding = true;
    transmitter.sendNECRepeat();
    forwarding = false;
    logger.resetSignalInterval();
    finishForwardRecord(idx, seq, FORWARD_RESULT_SENT, (uint16_t)(millis() - t0));

    // 续上窗口：按住期间每一次重复都把窗口往后推，松手后自然失效
    lastMatchedMs       = millis();
    lastRepeatForwardMs = lastMatchedMs;
    return true;
}

int IRForwarder::getForwardRecords(ForwardRecord* out, int maxCount) const {
    if (out == nullptr || maxCount <= 0) return 0;
    int n = (forwardRecordCount < maxCount) ? forwardRecordCount : maxCount;
    for (int i = 0; i < n; i++) {
        // 倒序：i=0 取最新那条
        int idx = (int)forwardRecordIndex - 1 - i;
        while (idx < 0) idx += MAX_FORWARD_RECORDS;
        out[i] = forwardRecords[idx];
    }
    return n;
}

void IRForwarder::clearForwardRecords() {
    forwardRecordIndex = 0;
    forwardRecordCount = 0;
    for (int i = 0; i < MAX_FORWARD_RECORDS; i++) {
        forwardRecords[i] = ForwardRecord();
    }
}

void IRForwarder::update() {
    // 处理延时状态机
    processDelayState();
    
    // 如果延时完成且有待转发任务，执行转发
    if (delayState == FORWARD_DELAY_READY) {
        // 时序码 B 走这条路：延时结束后才回查规则。
        // 以前这段是在接收任务里**就地同步**发完的（几十~几百毫秒），
        // 主循环的 OTA / MQTT / 网页 / LED 全部跟着停摆。详见 forwardSignal()。
        if (pendingTimingRuleId >= 0) {
            uint8_t ruleId  = (uint8_t)pendingTimingRuleId;
            uint8_t recIdx  = pendingTimingRecordIndex;
            uint16_t recSeq = pendingTimingRecordSeq;
            pendingTimingRuleId = -1;
            delayState = FORWARD_DELAY_IDLE;

            IR_MapRule* rule = mapper.findRuleById(ruleId);
            if (rule == nullptr) {
                // 这 20ms 里规则被删/被改了。记 ID 而不是裸指针，正是为了这里
                // 能安全地判失败，而不是拿着悬空指针去发一串垃圾。
                // latencyMs 保持 0 —— 压根没走到发射，别写出一个伪造的"延迟"。
                finishForwardRecord(recIdx, recSeq, FORWARD_RESULT_FAILED, 0);
            } else {
                markSendStart(recIdx, recSeq);
                unsigned long t0 = millis();
                bool ok = executeForward(*rule);
                finishForwardRecord(recIdx, recSeq,
                                    ok ? FORWARD_RESULT_SENT : FORWARD_RESULT_FAILED,
                                    (uint16_t)(millis() - t0));
            }
            return;  // 本帧已执行转发，不再从队列取任务
        }

        if (pendingTaskValid) {
            const ForwardTask& t = pendingTask;
            // 边界检查：确保 signal_count 有效且数组访问安全
            if (t.signal_count == 1 && t.b_protocols[0] == 1) {
                markSendStart(t.record_index, t.record_seq);
                forwarding = true;
                unsigned long t0 = millis();
                if (t.intervals[0] > 0) delayPrecise(t.intervals[0]);
                sendNecOrRepeat(t.signal_data[2]);
                forwarding = false;
                logger.resetSignalInterval();
                finishForwardRecord(t.record_index, t.record_seq, FORWARD_RESULT_SENT,
                                    (uint16_t)(millis() - t0));
            } else if (t.signal_count > 0 && t.signal_count <= MAX_B_SIGNALS_PER_RULE) {
                IR_MapRule rule;
                if (buildRuleFromTask(t, rule)) {
                    markSendStart(t.record_index, t.record_seq);
                    unsigned long t0 = millis();
                    bool ok = executeForward(rule);
                    finishForwardRecord(t.record_index, t.record_seq,
                                        ok ? FORWARD_RESULT_SENT : FORWARD_RESULT_FAILED,
                                        (uint16_t)(millis() - t0));
                }
            }
            pendingTaskValid = false;
            delayState = FORWARD_DELAY_IDLE;
            return;  // 本帧已执行转发，不再从队列取任务
        }

        // READY 但槽位是空的（待发任务被后来的信号顶掉了）：把状态机收回 IDLE。
        // 以前这里只判断 `READY && pendingTaskValid`，槽位空时不会走到这个分支，
        // delayState 就一直停在 READY —— 语义上等于"下一次 update 还会再进来一次"。
        delayState = FORWARD_DELAY_IDLE;
    }

    // 优化：如果正在转发，快速返回，避免阻塞
    if (forwarding) {
        return;
    }

    // 队列里的任务也不能在上一次发射还没结束 / 还在静默期时抢着发。
    // 这条路以前完全没有这道闸：上一帧刚发完，下一个 5ms tick 就把队列里的
    // 下一条接着发出去，FORWARD_DELAY_BEFORE_SEND_MS 的 AB 隔离和发射后的
    // 静默期形同虚设（评审 #5 说的"update() 入口也要挡"，之前只在
    // forwardSignal() 里挡了）。留在队列里等静默期过去再取，队列只有 3 深，
    // 不会无限堆积；isBusyTransmitting() 也不可能一直为真（发射结束 + 80ms
    // 后必然转 false），不存在把队列卡死的可能。
    if (isBusyTransmitting()) {
        return;
    }

    // 从消息队列获取转发任务
    ForwardTask task;
    if (queue.pop(task) && task.valid) {
        // 边界检查：确保 signal_count 有效且数组访问安全
        if (task.signal_count == 1 && task.b_protocols[0] == 1) {
            markSendStart(task.record_index, task.record_seq);
            forwarding = true;
            unsigned long t0 = millis();
            if (task.intervals[0] > 0) delayPrecise(task.intervals[0]);
            sendNecOrRepeat(task.signal_data[2]);
            forwarding = false;
            logger.resetSignalInterval();
            finishForwardRecord(task.record_index, task.record_seq, FORWARD_RESULT_SENT,
                                (uint16_t)(millis() - t0));
        } else if (task.signal_count > 0 && task.signal_count <= MAX_B_SIGNALS_PER_RULE) {
            IR_MapRule rule;
            if (buildRuleFromTask(task, rule)) {
                markSendStart(task.record_index, task.record_seq);
                unsigned long t0 = millis();
                bool ok = executeForward(rule);
                finishForwardRecord(task.record_index, task.record_seq,
                                    ok ? FORWARD_RESULT_SENT : FORWARD_RESULT_FAILED,
                                    (uint16_t)(millis() - t0));
            }
        }
    }
}

bool IRForwarder::forwardSignal(const UnifiedSignal& aSignal) {
    decodeMs = millis();   // 延迟探针起点：解码刚完成、进到转发器这一刻

    if (!enabled) {
        addForwardRecord(aSignal, -1, 0, FORWARD_RESULT_DISABLED);
        return false;  // 转发功能已禁用
    }

    // 自收回声：TX 后 TX_ECHO_WINDOW_MS 内解出的、码值等于刚发出去的那条 B 码
    // ⇒ 是我自己发出的光的回波，不是遥控器按的。
    // 既不转发也不记记录（记录里出现回环会严重误导排障）；顺带挡住
    // "某条规则的 B 恰好等于另一条规则的 A" 时的级联转发。
    // ⚠️ 判据是**码值**不是时间：真实按键的 A 码与被转发的 B 码是不同值，不会误伤。
    if (!aSignal.useTiming &&
        transmitter.isEchoOfLastTx(aSignal.data, TX_ECHO_WINDOW_MS)) {
        return false;
    }

    // ⚠️ 时序码信号的回声挡不住（v1.3.24 新增时序码兜底后才有这个问题）
    // isEchoOfLastTx 比的是**码值**，而时序码没有 data、只有 Base64 ⇒ 上面那条
    // 判据天然不适用。这里改按**时间**挡：刚发完 TX_ECHO_WINDOW_MS 内到达的
    // 时序码，一律当作自家余光。
    // 代价：遥控器紧跟在转发后 200ms 内按下，那一下会被误挡。
    // 可接受 —— 转发场景下"遥控器和被控设备同时收到"本来就是冲突场景，
    // 而漏挡回声会导致自发自收回环甚至级联转发，后果严重得多。
    //
    // ⚠️⚠️ 必须加"是否真的发射过"这个守卫（v1.3.24 审查时发现）：
    // msSinceTxEnd() 的实现是 `millis() - lastTxEndMs`，而 lastTxEndMs 初始为 0
    // ⇒ **上电后从未发射时它返回 millis()**，在上电头 200ms 内就是一个**小值**。
    // 那时若不做判断，一个真实的按键会被当成"刚发完的余光"静默丢弃
    // （表现为"刚重启就按遥控器没反应，重启一秒后就好了"）。
    // 对照：isEchoOfLastTx 就有 `lastTxCode == 0` 这道守卫，这里同样要有。
    if (aSignal.useTiming && transmitter.hasEverTransmitted() &&
        transmitter.msSinceTxEnd() < TX_ECHO_WINDOW_MS) {
        return false;
    }

    // 防御性守卫：主循环已经用 isBusyTransmitting() 挡过一道，这里再挡一次，
    // 保护其它调用方（以及将来新增的入口）。发射刚结束时收到的多半是我们自己
    // 发出的余光 —— 拿它去匹配规则就是自发自收回环，甚至级联转发。
    // 静默期内的信号既不转发也不记记录（记录里出现回环会严重误导排障）。
    if (isBusyTransmitting()) {
        return false;
    }

    // 通用 NEC 遥控器按住不放时，后续发的是**重复码 0xFFFFFFFF**（不带键值），
    // 按 A 码匹配不到任何规则 ⇒ 按住只有第一帧会被转发（实测：注入重复码得到
    // result=1/NOMATCH）。这里用"最近命中的规则 + 时间窗"把它续上。
    if (!aSignal.useTiming && aSignal.protocol == 1 && aSignal.data == NEC_REPEAT_CODE) {
        // 续发成功就直接返回（forwardRepeat 内部已刷新窗口）。
        // ⚠️ 没续发（窗口过期 / 规则被删 / B 是多信号序列）时**必须继续往下走**：
        //    重复码本来就匹配不到任何规则，会落到下面的 NOMATCH 分支 —— 那条记录
        //    得在，否则排障时会显示成"被挡下了"，而真实原因是"这个重复码不属于任何键"。
        if (forwardRepeat(aSignal)) return true;
    }

    // 查规则并转发（重复码已在上方分流）
    IR_MapRule* rule = mapper.findRule(aSignal);

    // 记下"最近命中的规则 + 时刻"，供上方的重复码续发用。
    // ⚠️ 未命中也要写（写 -1）：这样"按了个没配规则的键"会立刻清掉窗口，
    //    后面来的重复码不会误续到上一条规则上。
    lastMatchedRuleId = (rule != nullptr) ? (int16_t)rule->id : -1;
    lastMatchedMs     = millis();

    if (rule == nullptr) {
        // 排障第一现场：收到信号但没有任何规则匹配 —— 界面上要能一眼看到这条
        addForwardRecord(aSignal, -1, 0, FORWARD_RESULT_NOMATCH);
        return false;  // 未找到规则，不转发
    }
    
    // 匹配成功：先落一条记录，发射结果与耗时由发射路径回填（finishForwardRecord）。
    // 记录下标与身份号随任务一路带到发射点 —— 发射可能延后（延时槽 / 队列），
    // 届时"最新一条记录"未必还是这一条。
    uint8_t recIdx = addForwardRecord(aSignal, (int16_t)rule->id, rule->bSequence.signalCount,
                                     FORWARD_RESULT_SENT);
    uint16_t recSeq = forwardRecords[recIdx].seq;

    // 含时序码 B 信号的规则走"只记规则 ID"的延迟路径。三个理由：
    //   ① 以前这里是**就地同步**发完整条序列，而且就发生在**接收任务**里
    //      （main.cpp 的 10ms 红外接收分支）—— 那一次迭代被整个吞掉，A 之后的
    //      irParser.resumeReceiver() / irReceiver.update() 都得等它发完才轮到。
    //      ⚠️ 别把这条读成"消除了停摆"：发射本身（sendSequence → IRsend::sendRaw）
    //      是阻塞的，挪进发射任务分支后 loop() **照样**停摆同样长的时间 ——
    //      loop() 是串行的，在哪个分支阻塞都一样。2026-09-29 实测：一条 261ms 的
    //      时序码规则，发射窗口内打 /api/status 要等 192ms 才回来。
    //      （所以本项的收益是"接收侧不被占用"，不是"主循环不被占用"。）
    //   ② 时序点阵最多 MAX_LEARN_TIMING_COUNT 点（单条信号），ForwardTask 只存协议码值
    //      （4×(2+6) 字节数组），塞不进 3 深队列 ⇒ 也不能像 NEC 那样"入队等别人发"。
    //   ③ 顺带补上 FORWARD_DELAY_BEFORE_SEND_MS（20ms AB 隔离）：同一条规则，
    //      B 是 NEC 码走异步链路有这 20ms、B 是时序码走同步链路却没有 ——
    //      两种波形的发射条件不一致，是"学到的码转发时好时坏"的一个来源。
    for (int i = 0; i < rule->bSequence.signalCount; i++) {
        if (!rule->bSequence.signals[i].useTiming) continue;

        // 这个延时槽是**单占用**的（delaying / 待发的任务 / 待发的时序任务共用一个）。
        // startDelay 返回 false = 已经有一个延时在跑：那就顶掉它（以最新的为准）。
        // 时序序列本身很长，再排队只会越拖越久；而且两次解码之间最短也隔着一整帧
        // （NEC 67.5ms），实际上到不了这里。
        delayTimer.startDelay(FORWARD_DELAY_BEFORE_SEND_MS);
        pendingTaskValid = false;          // 让出同一个槽
        pendingTimingRuleId = (int16_t)rule->id;
        pendingTimingRecordIndex = recIdx;
        pendingTimingRecordSeq   = recSeq;
        delayState = FORWARD_DELAY_WAITING;
        return true;
    }
    
    // 创建转发任务
    ForwardTask task;
    if (!createForwardTask(*rule, task)) {
        finishForwardRecord(recIdx, recSeq, FORWARD_RESULT_FAILED, 0);
        return false;
    }
    task.record_index = recIdx;
    task.record_seq   = recSeq;
    
    // 开始非阻塞延时
    if (delayTimer.startDelay(FORWARD_DELAY_BEFORE_SEND_MS)) {
        delayState = FORWARD_DELAY_WAITING;
        pendingTask = task;
        pendingTaskValid = true;
        pendingTimingRuleId = -1;   // 同上：延时槽单占用，让出时序那条路
        return true;
    } else {
        // 延时启动失败，直接通过队列转发
        if (!queue.push(task)) {
            logger.log("warning", "转发队列已满，丢弃任务");
            finishForwardRecord(recIdx, recSeq, FORWARD_RESULT_FAILED, 0);
            return false;
        }
        return true;
    }
}

bool IRForwarder::testForward(const UnifiedSignal& aSignal) {
    // 试跑：不看总开关、不走延时，立刻匹配并发射 —— 专用于"这条规则到底通不通"的即时验证
    IR_MapRule* rule = mapper.findRule(aSignal);
    if (rule == nullptr) {
        addForwardRecord(aSignal, -1, 0, FORWARD_RESULT_NOMATCH);
        return false;
    }

    uint8_t recIdx = addForwardRecord(aSignal, (int16_t)rule->id, rule->bSequence.signalCount,
                                     FORWARD_RESULT_SENT);
    uint16_t recSeq = forwardRecords[recIdx].seq;
    markSendStart(recIdx, recSeq);
    unsigned long t0 = millis();
    bool ok = executeForward(*rule);
    finishForwardRecord(recIdx, recSeq, ok ? FORWARD_RESULT_SENT : FORWARD_RESULT_FAILED,
                        (uint16_t)(millis() - t0));
    return ok;
}

bool IRForwarder::testForwardPayload(const String& payload) {
    String trimmed = payload;
    trimmed.trim();
    if (trimmed.length() == 0) return false;

    // 只处理字符串形式（"1,0x00003039"），面板与接口用的就是它
    SignalSequence seq;
    if (!IRLearner::parseStringSequence(trimmed, seq, timingScratch(), TIMING_SCRATCH_CAPACITY)) {
        return false;
    }
    if (seq.signalCount == 0) return false;

    // A 侧本来就是单信号，取序列第一个去匹配
    return testForward(seq.signals[0]);
}

bool IRForwarder::forwardSignalViaPayload(const String& payload) {
    String trimmed = payload;
    trimmed.trim();
    if (trimmed.length() == 0) return false;

    SignalSequence seq;
    if (!IRLearner::parseStringSequence(trimmed, seq, timingScratch(), TIMING_SCRATCH_CAPACITY)) {
        return false;
    }
    if (seq.signalCount == 0) return false;

    // 走真实入站路径：20ms AB 隔离、忙碌闸门、队列全部生效 —— 这才是遥控器按下时的那条路
    return forwardSignal(seq.signals[0]);
}

void IRForwarder::setEnabled(bool enabled) {
    this->enabled = enabled;
}

bool IRForwarder::isEnabled() const {
    return enabled;
}

bool IRForwarder::sendSequence(const SignalSequence& sequence) {
    if (!sequence.isValid() || sequence.signalCount == 0) {
        return false;
    }

    // ── 告诉看门狗"刚**发射**过红外"（v1.3.55）──────────────────────────────
    // 这里是**所有**发射路径的唯一汇合点（面板 /api/signals/send、MQTT send_signal、
    // /api/forward/test、HA 入站、遥控器转发都走它），所以只挂这一处就够。
    //   ① 探测避让：发射会占住接收（这是一段聋期），与"刚收过红外"同理；
    //   ② 记进僵死快照 ⇒ 僵死时能区分"是发射触发的"还是"跟红外无关"。
    // 只写一个 uint32，⛔ 不开堆、不 Serial（发射路径的硬约束）。
    NetWatchdog::noteIrSent(millis());

    // 使用序列中第一个信号的repeatCount（如果>1，重复发送整个序列）
    uint8_t repeatCount = 1;
    if (sequence.signalCount > 0 && sequence.signals[0].repeatCount > 1) {
        repeatCount = sequence.signals[0].repeatCount;
        if (repeatCount > SIGNAL_REPEAT_MAX) {
            repeatCount = SIGNAL_REPEAT_MAX;
        }
    }

    // ── 发射埋点（v1.3.30，只加日志、不改行为）─────────────────────────────
    // 目的：定位"反复发送把设备打进僵死态、最后硬件看门狗复位"。
    //   ① 进入/退出各一行 ⇒ 若只有"进入"没有"退出"，说明**卡在发射里**；
    //   ② 带上 freeHeap/maxBlock ⇒ 分辨是"内存"还是"时间"问题；
    //   ③ transmit() 另打 yield 次数 ⇒ 看关中断窗口到底让出去几次。
    // ⚠️ 每行一次 Serial.printf 是同步忙等（~4ms/行），只放在**每次发送**一层，
    //   不进 per-point 循环，避免日志本身变成阻塞源。
    // ── 发射结果统计（v1.3.44）────────────────────────────────────────────
    // 以前 sendSequence() 无条件 return true ⇒ “一条都没发出去”也照样回成功，
    // 界面显示「已发送」而被控设备毫无反应（2026-10-04 实机踩到）。现在如实统计。
    uint16_t txSent   = 0;
    uint16_t txFailed = 0;

    const unsigned long _txT0    = millis();
    const uint32_t      _txHeap0 = ESP.getFreeHeap();
    const uint32_t      _txBlk0  = ESP.getMaxFreeBlockSize();
    Serial.printf_P(PSTR("[TX] start rep=%u sig=%u timing=%u heap=%u blk=%u\n"),
                  (unsigned)repeatCount, (unsigned)sequence.signalCount,
                  (unsigned)(sequence.signals[0].useTiming ? 1 : 0),
                  (unsigned)_txHeap0, (unsigned)_txBlk0);
    
    // 检查第一个信号是否使用时序码
    bool useTiming = (sequence.signalCount > 0 && sequence.signals[0].useTiming);
    
    // 检查第一个信号是否为NEC协议（且不使用时序码）
    bool isNEC = (sequence.signalCount > 0 && 
                  !sequence.signals[0].useTiming &&
                  sequence.signals[0].isProtocol() && 
                  sequence.signals[0].protocol == 1);
    
    // 获取间隔数量，判断是否有第一个信号前的间隔
    uint8_t intervalCount = sequence.getIntervalCount();
    bool hasFirstInterval = (intervalCount == sequence.signalCount);
    
    if (useTiming) {
        // 使用时序码：直接发送时序码，包含精确的延迟
        //
        // ⚠️⚠️ v1.3.44 核心修复：改用**常驻**的 timingBuffer（与下面“其他协议或序列”
        //   分支同一套写法），不再 signalCopy.loadTiming()。
        //   旧写法在请求内部 new IRTiming[点数]（8 B/点 ⇒ 168 点要 2368 B 连续堆），
        //   另加 UnifiedSignal 深拷贝一份 Base64（~448 B）。而设备“学完指令”后空闲堆
        //   会从 ~12 KB **永降**到 ~5 KB（一次性台阶，回不去，见 memory/MEMORY.md），
        //   请求期间只剩 3.9 KB ⇒ 每次都被内存闸拦住，**且失败是静默的**：
        //   loadTiming() 返回 false 被丢弃，sendSequence() 照样 return true
        //   ⇒ API 回 "Signal sent"、界面显示“已发送”，而被控设备毫无反应。
        //   实测（2026-10-04，111）：120/144/152 点能发，**160 点起 100% 静默失败**。
        //   timingBuffer 是成员数组（常驻 .bss，不占堆），上限 MAX_LEARN_TIMING_COUNT=200，
        //   而学习/解析侧按同一常量截断 ⇒ 实际信号必然装得下。
        //   ⛔ 手粘的超 200 点 base64 会被 base64ToTiming 截断（那条路现在只剩它）。
        for (uint8_t repeat = 0; repeat < repeatCount; repeat++) {
            // 如果有第一个信号前的间隔，先等待
            if (hasFirstInterval && sequence.intervals[0] > 0) {
                delayPrecise(sequence.intervals[0]);
            }
            
            // 发射每个信号
            for (uint8_t i = 0; i < sequence.signalCount; i++) {
                const UnifiedSignal& signal = sequence.signals[i];
                
                if (signal.useTiming) {
                    if (signal.hasTiming()) {
                        int timingCount = 0;
                        if (compileSignal(signal, timingBuffer, timingCount) && timingCount > 0) {
                            transmitter.transmit(timingBuffer, timingCount);
                            txSent++;
                        } else {
                            // 装配失败必须留痕：这就是“界面说已发送、实际一个字节没发”的现场
                            txFailed++;
                            Serial.printf_P(PSTR("[TX] 时序码未装配 heap=%u blk=%u b64=%u\n"),
                                          (unsigned)ESP.getFreeHeap(),
                                          (unsigned)ESP.getMaxFreeBlockSize(),
                                          (unsigned)signal.timingBase64.length());
                        }
                    } else {
                        // ⚠️ “该发但信号本身是空的”（useTiming 为真、却没有 base64 或点数）。
                        //   这是 parseStringSequence() 在低堆下编码失败的残留形态：旧写法直接
                        //   跳过、而 sendSequence() 又无条件 return true ⇒ 界面显示“已发送”、
                        //   被控设备毫无反应。现在计入失败（2026-10-04 实测 200 点踩到）。
                        txFailed++;
                        Serial.printf_P(PSTR("[TX] 时序码为空 heap=%u blk=%u pts=%d\n"),
                                      (unsigned)ESP.getFreeHeap(),
                                      (unsigned)ESP.getMaxFreeBlockSize(),
                                      (int)signal.timingCount);
                    }
                }
                
                // 信号间隔
                if (hasFirstInterval) {
                    // intervals[0] 是第一个信号前的间隔，intervals[i+1] 是 signals[i] 和 signals[i+1] 之间的间隔
                    if (i < sequence.signalCount - 1) {
                        delayPrecise(sequence.intervals[i + 1]);
                    }
                } else {
                    // 标准模式：intervals[i] 是 signals[i] 和 signals[i+1] 之间的间隔
                    if (i < sequence.signalCount - 1) {
                        delayPrecise(sequence.intervals[i]);
                    }
                }
            }
            
            // 重复间隔（最后一次重复后不需要间隔）
            // ⚠️ 时序码**不能**用那 100ms 默认值（v1.3.26）：
            //   原遥控器是紧挨着连发 7~12 遍同一帧，帧间隔实测 7,270µs。
            //   被控设备的接收端就是按这个节奏判重/去抖的，拉到 100ms（14 倍）
            //   等于把"一串突发"拆成一条条孤立帧 ⇒ 风扇不认。
            if (repeat < repeatCount - 1) {
                delayMicroseconds(TIMING_BURST_GAP_US);
            }
        }
    } else if (isNEC && sequence.signalCount == 1) {
        // NEC协议单个信号：检查是否为重复码
        const UnifiedSignal& signal = sequence.signals[0];
        
        // 如果有第一个信号前的间隔，先等待
        if (hasFirstInterval && sequence.intervals[0] > 0) {
            delayPrecise(sequence.intervals[0]);
        }
        
        if (signal.data == NEC_REPEAT_CODE) {
            // 这是NEC重复码，直接发送重复码
            transmitter.sendNECRepeat();
        } else {
            // 完整信号：如果信号自身的repeatCount>1，发送1次完整信号 + (repeatCount-1)次重复码
            // 按照NEC协议标准：完整信号后110ms发送重复码，重复码之间间隔110ms
            uint8_t signalRepeatCount = signal.repeatCount;
            if (signalRepeatCount == 0) signalRepeatCount = 1;
            if (signalRepeatCount > SIGNAL_REPEAT_MAX) signalRepeatCount = SIGNAL_REPEAT_MAX;
            
            transmitter.sendNECShort(signal.data, 32);
            
            // 发送重复码（如果repeatCount > 1）
            if (signalRepeatCount > 1) {
                for (uint8_t r = 1; r < signalRepeatCount; r++) {
                    delay(110);  // NEC协议标准间隔：110ms
                    transmitter.sendNECRepeat();
                }
            }
        }
    } else if (isNEC && sequence.signalCount > 1) {
        // NEC协议多信号序列：需要特殊处理重复码
        for (uint8_t repeat = 0; repeat < repeatCount; repeat++) {
            // 如果有第一个信号前的间隔，先等待
            if (hasFirstInterval && sequence.intervals[0] > 0) {
                delayPrecise(sequence.intervals[0]);
            }
            
            // 发射每个信号
            for (uint8_t i = 0; i < sequence.signalCount; i++) {
                const UnifiedSignal& signal = sequence.signals[i];
                
                // 检查是否为NEC重复码
                if (signal.data == NEC_REPEAT_CODE) {
                    // 发送NEC重复码
                    transmitter.sendNECRepeat();
                } else {
                    // 发送完整NEC信号
                    transmitter.sendNEC(signal.data, 32);
                }
                
                // 信号间隔
                if (hasFirstInterval) {
                    // intervals[0] 是第一个信号前的间隔，intervals[i+1] 是 signals[i] 和 signals[i+1] 之间的间隔
                    if (i < sequence.signalCount - 1) {
                        delayPrecise(sequence.intervals[i + 1]);
                    }
                } else {
                    // 标准模式：intervals[i] 是 signals[i] 和 signals[i+1] 之间的间隔
                    if (i < sequence.signalCount - 1) {
                        delayPrecise(sequence.intervals[i]);
                    }
                }
            }
            
            // 重复间隔（最后一次重复后不需要间隔）
            if (repeat < repeatCount - 1) {
                delayPrecise(100);  // 默认100ms重复间隔
            }
        }
    } else {
        // 其他协议或序列：使用自定义时序生成
        for (uint8_t repeat = 0; repeat < repeatCount; repeat++) {
            // 如果有第一个信号前的间隔，先等待
            if (hasFirstInterval && sequence.intervals[0] > 0) {
                delayPrecise(sequence.intervals[0]);
            }
            
            // 发射每个信号
            for (uint8_t i = 0; i < sequence.signalCount; i++) {
                const UnifiedSignal& signal = sequence.signals[i];
                
                int timingCount = 0;
                if (compileSignal(signal, timingBuffer, timingCount) && timingCount > 0) {
                    transmitter.transmit(timingBuffer, timingCount);
                    txSent++;
                } else {
                    // 协议信号装配失败（生成器不支持 / 内存不够）也必须留痕
                    txFailed++;
                    Serial.printf_P(PSTR("[TX] 协议信号未装配 proto=%u data=0x%08X heap=%u\n"),
                                  (unsigned)signal.protocol, (unsigned)signal.data,
                                  (unsigned)ESP.getFreeHeap());
                }
                
                // 信号间隔
                if (hasFirstInterval) {
                    // intervals[0] 是第一个信号前的间隔，intervals[i+1] 是 signals[i] 和 signals[i+1] 之间的间隔
                    if (i < sequence.signalCount - 1) {
                        delayPrecise(sequence.intervals[i + 1]);
                    }
                } else {
                    // 标准模式：intervals[i] 是 signals[i] 和 signals[i+1] 之间的间隔
                    if (i < sequence.signalCount - 1) {
                        delayPrecise(sequence.intervals[i]);
                    }
                }
            }
            
            // 重复间隔（最后一次重复后不需要间隔）
            if (repeat < repeatCount - 1) {
                delayPrecise(100);  // 默认100ms重复间隔
            }
        }
    }
    
    logger.resetSignalInterval();  // 发送完成后重置信号间隔

    // ── 发射埋点收尾：能走到这里说明**发射本身完整跑完** ────────────────
    // 若串口只见 start 不见 end ⇒ 卡在发射路径内部（不是内存问题，是时间/中断问题）。
    {
        const unsigned long ms = millis() - _txT0;
        const uint32_t h = ESP.getFreeHeap();
        const uint32_t b = ESP.getMaxFreeBlockSize();
        Serial.printf_P(PSTR("[TX] end ms=%lu heap=%u(%+ld) blk=%u(%+ld) sent=%u failed=%u\n"),
                      ms, (unsigned)h, (long)h - (long)_txHeap0,
                      (unsigned)b, (long)b - (long)_txBlk0,
                      (unsigned)txSent, (unsigned)txFailed);
    }
    // ⚠️ v1.3.44：以前无条件 return true ⇒ “一条都没发出去”也回成功。
    //   现在：全部失败才返回 false（部分成功仍算成功，但串口有 failed 计数）。
    //   ⛔ 别退回无条件 true —— 那正是“界面显示已发送、设备毫无反应”的根源。
    return (txFailed == 0) || (txSent > 0);
}

bool IRForwarder::sendSignal(const UnifiedSignal& signal, uint8_t repeatCount) {
    // 兼容层：将单个信号转换为序列，然后调用 sendSequence
    // 将repeatCount设置到信号的repeatCount字段
    UnifiedSignal signalCopy = signal;
    if (repeatCount > 0 && repeatCount <= SIGNAL_REPEAT_MAX) {
        signalCopy.repeatCount = repeatCount;
    }
    
    // 对于单个NEC信号，直接使用快速路径，避免序列转换开销
    if (signalCopy.isProtocol() && signalCopy.protocol == 1 && !signalCopy.useTiming && signalCopy.repeatCount == 1) {
        // 单个NEC信号快速路径：直接发送，避免序列转换开销
        transmitter.sendNEC(signalCopy.data, 32);
        logger.resetSignalInterval();
        return true;
    }
    
    // 其他情况：转换为序列发送
    SignalSequence sequence;
    sequence.clear();
    sequence.addSignal(signalCopy, 0, false);
    return sendSequence(sequence);
}

bool IRForwarder::isForwarding() const {
    return forwarding || transmitter.isTransmitting();
}

void IRForwarder::sendNecOrRepeat(uint32_t data) {
    if (data == NEC_REPEAT_CODE) {
        transmitter.sendNECRepeat();
    } else {
        // 单信号 NEC 转发走"不补长"版本：波形不变，聋期少 43.7ms（见 IRTransmitter::sendNECShort）
        transmitter.sendNECShort(data, 32);
    }
}

bool IRForwarder::isBusyTransmitting() const {
    // ⚠️⚠️ msSinceTxEnd() 必须配 hasEverTransmitted() 这道守卫（v1.3.24 审查时发现）：
    //   它的实现是 `millis() - lastTxEndMs`，而 lastTxEndMs 初始为 0
    //   ⇒ **上电后从未发射过时返回的是 millis() 本身**。
    //   上电头 TX_SILENCE_AFTER_SEND_MS(80ms) 内 millis() 很小 ⇒ 本函数恒为 true
    //   ⇒ main.cpp 的 `if (isBusyTransmitting()) resumeReceiver();` 会把这段窗口内
    //   收到的信号全部丢弃 —— 现象是"刚上电就按遥控器没反应，过一秒就好了"。
    //   这个 bug 比本函数早存在，但此前毫发无损（没人会在上电 80ms 内按键）；
    //   现在时序码兜底上线、按键场景变多，必须补上，否则会误判成"新功能坏了"。
    return forwarding || transmitter.isTransmitting() ||
           (transmitter.hasEverTransmitted() &&
            transmitter.msSinceTxEnd() < TX_SILENCE_AFTER_SEND_MS);
}

bool IRForwarder::sendRawTiming(const IRTiming* timing, int count, uint8_t repeatCount) {
    if (timing == nullptr || count <= 0 || repeatCount == 0) {
        return false;
    }
    
    for (uint8_t i = 0; i < repeatCount; i++) {
        if (!transmitter.transmit(timing, count)) {
            return false;
        }
        if (i < repeatCount - 1) {
            delay(100);  // 重复发送之间的延迟
        }
    }
    
    logger.resetSignalInterval();  // 发送完成后重置信号间隔
    return true;
}

bool IRForwarder::compileSignal(const UnifiedSignal& signal, IRTiming* timing, int& count) {
    // 统一使用时序生成器编译信号
    // 对于协议信号，使用IRGenerator生成时序
    // 对于时序码信号，从Base64加载时序
    
    if (timing == nullptr) {
        count = 0;
        return false;
    }
    
    if (signal.useTiming) {
        // 时序码信号：从Base64加载时序
        // 上限用 MAX_LEARN_TIMING_COUNT，与 timingBuffer 的容量同源（见 ir_forwarder.h 的说明）
        return SignalUtils::base64ToTiming(signal.timingBase64, timing, count, MAX_LEARN_TIMING_COUNT);
    } else if (signal.isProtocol()) {
        // 协议信号：使用时序生成器生成时序（本生成器最长 67 点，远够）
        return generator.generateTiming(signal.protocol, signal.data, timing, count, MAX_LEARN_TIMING_COUNT);
    } else {
        count = 0;
        return false;
    }
}

bool IRForwarder::executeForward(const IR_MapRule& rule) {
    forwarding = true;
    // 「开始准备发射」的打点已上移到各调用点（markSendStart(记录下标, 身份号)）——
    // 它必须落在**具体那条记录**上，而这里拿不到记录下标。
    
    // 使用 sendSequence() 统一处理，使用序列中信号的repeatCount
    bool result = sendSequence(rule.bSequence);
    
    forwarding = false;
    logger.resetSignalInterval();  // 发送完成后重置信号间隔
    return result;
}

void IRForwarder::delayPrecise(unsigned long ms) {
    // 使用硬件定时器实现精准延时（误差≤20ms）
    PreciseTimer::delayPreciseMs(ms);
}

bool IRForwarder::parseAndSendSignal(const String& payload) {
    String trimmed = payload;
    trimmed.trim();
    
    if (trimmed.length() == 0) {
        return false;
    }
    
    // 检测格式：JSON或字符串
    bool isJSON = (trimmed.startsWith("{") && trimmed.endsWith("}"));
    
    if (isJSON) {
        // JSON格式：统一解析为Sequence格式
        size_t jsonSize = trimmed.length() + 256;
        if (jsonSize > 2048) jsonSize = 2048;
        if (jsonSize < 256) jsonSize = 256;
        
        // 加强内存检查：确保有足够的内存
        if (!MemoryMonitor::checkMemory(jsonSize + 512)) {
            return false;
        }
        
        DynamicJsonDocument doc(jsonSize);
        DeserializationError error = deserializeJson(doc, trimmed);
        
        if (error) {
            return false;
        }
        
        // 检查是否为序列格式（包含 signals 数组）
        if (doc.containsKey("signals")) {
            // 序列格式
            SignalSequence sequence;
            JsonObject docObj = doc.as<JsonObject>();
            if (!SignalUtils::sequenceFromJSON(docObj, sequence)) {
                return false;
            }
            return sendSequence(sequence);
        } else {
            // 不再支持单个信号格式，统一使用序列格式
            return false;
        }
    } else {
        // 字符串格式：支持序列格式
        SignalSequence sequence;
        if (!IRLearner::parseStringSequence(trimmed, sequence, timingScratch(), TIMING_SCRATCH_CAPACITY)) {
            return false;
        }
        return sendSequence(sequence);
    }
}

void IRForwarder::processDelayState() {
    if (delayState == FORWARD_DELAY_WAITING) {
        if (delayTimer.isDelayComplete()) {
            delayState = FORWARD_DELAY_READY;
        }
    }
}

// FORWARD_TASK_MAX_B 已移到 message_queue.h，与 MAX_B_SIGNALS_PER_RULE 同源，
// 不再有"第 4~6 个 B 信号被静默截断"的问题。

bool IRForwarder::buildRuleFromTask(const ForwardTask& task, IR_MapRule& rule) {
    rule.id = task.rule_id;
    rule.name[0] = '\0';
    rule.valid = true;
    rule.aSignal.clear();
    rule.aSignal.protocol = task.signal_data[0] & 0xFF;
    rule.aSignal.data = task.signal_data[1];
    rule.aSignal.useTiming = false;
    rule.aSignal.repeatCount = 1;
    rule.bSequence.clear();

    // 判断是否有第一个信号前的间隔：
    // hasFirstInterval=true 时，createForwardTask 存储 intervals[0..signal_count-1]（共 signal_count 个）
    // hasFirstInterval=false 时，createForwardTask 存储 intervals[0..signal_count-2]（共 signal_count-1 个）
    // 因此：如果 signal_count==1 且 intervals[0]>0，或 signal_count>1 且 intervals[signal_count-1]>0，说明是 hasFirstInterval 模式
    // 但更可靠的判断是：检查间隔数组是否填满（signal_count 个间隔）
    bool hasFirstInterval = false;
    if (task.signal_count == 1) {
        // 单信号：如果 intervals[0]>0，说明有第一个信号前的间隔
        hasFirstInterval = (task.intervals[0] > 0);
    } else if (task.signal_count > 1) {
        // 多信号：如果最后一个间隔位置有值，说明是 hasFirstInterval 模式
        int lastIntervalIdx = (task.signal_count <= FORWARD_TASK_MAX_B)
                                  ? (task.signal_count - 1) : (FORWARD_TASK_MAX_B - 1);
        hasFirstInterval = (task.intervals[lastIntervalIdx] > 0);
    }

    int n = (task.signal_count < FORWARD_TASK_MAX_B) ? task.signal_count : FORWARD_TASK_MAX_B;
    for (int i = 0; i < n; i++) {
        UnifiedSignal bSignal;
        bSignal.clear();
        bSignal.protocol = task.b_protocols[i];
        bSignal.data = task.signal_data[i + 2];
        bSignal.useTiming = false;
        // 重复次数要跟着任务走：以前这里恒为 1，规则里配的（以及学习时识别出的）
        // 重复次数在异步转发链路上被整体丢弃 ⇒ 试跑能重复、实际只发一次。
        // sendSequence() 取的是 signals[0].repeatCount，所以只回落到首个信号即可。
        bSignal.repeatCount = (task.repeat_count > 0) ? task.repeat_count : 1;
        uint16_t interval = 0;
        bool isFirstInterval = false;
        if (hasFirstInterval) {
            isFirstInterval = true;
            if (i == 0) {
                interval = task.intervals[0];
            } else if (i < FORWARD_TASK_MAX_B) {
                interval = task.intervals[i];
            }
        } else {
            if (i > 0 && i - 1 < FORWARD_TASK_MAX_B - 1) {
                interval = task.intervals[i - 1];
            }
        }
        rule.bSequence.addSignal(bSignal, interval, isFirstInterval);
    }
    rule.bSequence.valid = true;
    return rule.bSequence.signalCount > 0;
}

bool IRForwarder::createForwardTask(const IR_MapRule& rule, ForwardTask& task) {
    // 时序码 B 信号无法经 ForwardTask 还原，调用方应走同步 executeForward
    for (int i = 0; i < rule.bSequence.signalCount && i < MAX_B_SIGNALS_PER_RULE; i++) {
        if (rule.bSequence.signals[i].useTiming) {
            return false;
        }
    }
    // 使用signal_data[0]存储protocol，signal_data[1]存储data，避免24位截断
    task.signal_data[0] = rule.aSignal.protocol;  // protocol
    task.signal_data[1] = rule.aSignal.data;  // 完整的32位data
    // B 信号上限跟 FORWARD_TASK_MAX_B（= MAX_B_SIGNALS_PER_RULE，当前 6）对齐
    int signalCount = (rule.bSequence.signalCount <= FORWARD_TASK_MAX_B)
                          ? rule.bSequence.signalCount : FORWARD_TASK_MAX_B;
    task.signal_count = static_cast<uint8_t>(signalCount);
    task.rule_id = rule.id;  // 保存规则ID用于调试

    // 初始化 intervals 数组为 0，避免未初始化的垃圾值
    for (int i = 0; i < FORWARD_TASK_MAX_B; i++) {
        task.intervals[i] = 0;
    }

    // B信号数据从signal_data[2]开始存储，同时存储协议信息
    for (int i = 0; i < signalCount; i++) {
        if (rule.bSequence.signals[i].useTiming) {
            // 使用时序码：存储时序码Base64字符串的哈希（简化处理）
            task.signal_data[i + 2] = rule.bSequence.signals[i].timingBase64.length();  // 临时存储长度
        } else {
            // 使用协议：存储data
            task.signal_data[i + 2] = rule.bSequence.signals[i].data;
        }
        task.b_protocols[i] = rule.bSequence.signals[i].protocol;  // 存储B信号协议
    }
    
    // 存储间隔：检查是否有第一个信号前的间隔
    uint8_t intervalCount = rule.bSequence.getIntervalCount();
    bool hasFirstInterval = (intervalCount == rule.bSequence.signalCount);
    
    if (hasFirstInterval) {
        // 有第一个信号前的间隔：存储所有间隔（包括第一个信号前的间隔）
        for (int i = 0; i < intervalCount && i < FORWARD_TASK_MAX_B; i++) {
            task.intervals[i] = rule.bSequence.intervals[i];
        }
    } else {
        // 标准模式：只存储信号之间的间隔
        for (int i = 0; i < signalCount - 1 && i < FORWARD_TASK_MAX_B; i++) {
            task.intervals[i] = rule.bSequence.intervals[i];
        }
    }

    // 重复次数跟着规则走（以前恒为 1，规则里配的重复次数在异步链路上被丢弃）。
    // sendSequence() 读的是 signals[0].repeatCount，这里取同一个口径。
    task.repeat_count = (rule.bSequence.signalCount > 0 &&
                         rule.bSequence.signals[0].repeatCount > 0)
                            ? rule.bSequence.signals[0].repeatCount : 1;
    task.valid = true;
    
    return true;
}
