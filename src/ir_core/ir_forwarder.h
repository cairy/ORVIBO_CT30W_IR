#ifndef IR_FORWARDER_H
#define IR_FORWARDER_H

#include <Arduino.h>
#include "unified_signal.h"
#include "ir_mapper.h"
#include "ir_generator.h"
#include "hardware/ir_transmitter.h"
#include "utils/message_queue.h"
#include "config/constants.h"
#include "config/pins.h"
#include "utils/precise_timer.h"

// 前向声明
class Logger;

// 延时状态机
enum ForwardDelayState {
    FORWARD_DELAY_IDLE,      // 空闲状态
    FORWARD_DELAY_WAITING,   // 等待延时完成
    FORWARD_DELAY_READY      // 延时完成，可以转发
};

// 简化转发逻辑，不再需要重复码相关数据结构

// ── 转发记录（环形缓冲）────────────────────────────────────────────
// 回答"刚才那条信号到底转发没有、转到哪去了"。这是排障最常问的问题：
// 你以为它在转发，其实 A 根本没匹配上任何规则。
// 注意：红外是单向的，这里最多只能证明"匹配到了规则、发射动作已执行"，
// 无法证明被控设备真的响应了 —— 界面上必须把这一点说清楚。
// 只保留最近若干条，不落盘（重启清空）。每条 22 字节（含对齐），8 条共 176 字节。
#define MAX_FORWARD_RECORDS 8

enum ForwardResult : uint8_t {
    FORWARD_RESULT_SENT     = 0,   // 匹配到规则，发射动作已执行
    FORWARD_RESULT_NOMATCH  = 1,   // 收到信号但没有任何规则匹配
    FORWARD_RESULT_FAILED   = 2,   // 匹配到规则但发射/入队失败
    FORWARD_RESULT_DISABLED = 3    // 转发总开关关闭，信号被丢弃
};

struct ForwardRecord {
    unsigned long timestamp;   // millis()，≈ 解码完成的时刻（见 IRForwarder::decodeMs）
    // 记录的**身份号**（单调递增，由 IRForwarder::nextRecordSeq 发）。环写满后老记录会被
    // 后来的覆盖，光凭下标分不清"这一格还是不是当初那条"。发射任务带着写入时的序号，
    // 回填前比对一次；不一致就说明记录已被覆盖，放弃回填（宁缺勿错）。
    uint16_t seq;
    uint32_t aCode;            // A 侧码值（时序码时为 0）
    uint16_t durationMs;       // 发射耗时（毫秒）
    // 转发延迟：解码完成 → 转发器开始准备发射，毫秒。等于
    // (timestamp → sendStartMs) 这一段 —— AB 隔离延时（20ms）、发射任务的 5ms
    // 轮询间隔、以及"前面还有一次发射没结束在等"都算在里面。
    // 这是"按一下遥控，设备多久才开始发"的直接观测量，专门用来定位
    // "转发慢"到底是慢在等还是慢在发。0 = 这条记录没有走到发射（未命中/被挡）。
    uint16_t latencyMs;
    // ⚠️ 必须比规则 ID 的宽度大：IR_MapRule::id 是 uint8_t（1..255，见 allocateId()），
    // 而这里用 -1 当"未命中"哨兵。原先写 int8_t，于是 id > 127 时会溢出成负数 ——
    // 面板的 ruleNameById() 见负数就当成"未命中"、显示成 `命中规则 #-116` 而不是规则名，
    // 而这条记录正是排障第一现场。改 int16_t（+8 B，8 条记录 × 1 字节）后语义自洽。
    int16_t  ruleId;           // 命中的规则 ID；-1 = 未命中
    uint8_t  aProtocol;        // A 侧协议号；255 = 时序码
    uint8_t  bCount;           // B 侧信号个数
    uint8_t  result;           // ForwardResult
};

// 转发器
class IRForwarder {
public:
    IRForwarder(IRMapper& mapper, IRTransmitter& transmitter, MessageQueue& queue, Logger& logger);
    bool init();
    void update();
    bool forwardSignal(const UnifiedSignal& aSignal);
    bool isForwarding() const;
    // 此刻"红外通道正忙"：正在发射，或刚发射完还在静默期内。
    // 主循环用它决定要不要把解码出来的信号当真 —— IRsend 不关中断，
    // 发射期间 GPIO5 一直在采样，自己发出的光会被自己收到（自发自收回环）。
    bool isBusyTransmitting() const;
    void setEnabled(bool enabled);  // 启用/禁用转发功能
    bool isEnabled() const;          // 获取转发功能状态
    // 单条 NEC：0xFFFFFFFF 是重复码，必须走 sendNECRepeat()，
    // 否则会把重复码当成完整帧发（被控设备不认）。多信号路径已经这么判了，
    // 单信号快速路径以前漏了 ⇒ 同一个 B 在两种路径下波形不同。
    void sendNecOrRepeat(uint32_t data);

    bool sendSequence(const SignalSequence& sequence);  // 发送信号序列（统一接口，使用序列中信号的repeatCount）
    bool sendSignal(const UnifiedSignal& signal, uint8_t repeatCount = 1);  // 直接发送信号（兼容层，内部调用sendSequence）
    bool sendRawTiming(const IRTiming* timing, int count, uint8_t repeatCount = 1);  // 发送原始时序数据
    bool parseAndSendSignal(const String& payload);  // 统一解析和发送信号（供API使用）

    // 试跑：跳过"转发总开关"和延时，立刻匹配规则并发射，用于「这条规则到底通不通」的即时验证。
    // 返回 true = 匹配到规则且发射成功。执行结果同样会写进转发记录。
    bool testForward(const UnifiedSignal& aSignal);
    bool testForwardPayload(const String& payload);  // 字符串形式（如 "1,0x00003039"），供 HTTP 接口直接调用
    // 同 testForwardPayload，但走**真实入站路径**（forwardSignal：含 20ms AB 隔离、
    // 忙碌闸门、队列）。专用于"测转发延迟"：/api/forward/test 默认走 testForward()
    // （不看总开关、不走延时、立刻发），量不出真实链路的时间。
    bool forwardSignalViaPayload(const String& payload);

    // 取转发记录（倒序：最新在前），返回实际条数
    int getForwardRecords(ForwardRecord* out, int maxCount) const;
    void clearForwardRecords();
    // 已写入过的记录**总数**（单调递增，uint16 回绕）。调用方在动作前后各取一次、
    // 比对是否变化，就能判断"这次动作到底有没有产生新记录" —— 用来区分
    // "被忙碌闸门/回声判据挡掉了"（无新记录）与"真的发出去了"。
    // 别再拿"最新一条记录的时长"去猜，那个判不准（2026-10-01 实测：8 次注入里
    // 被挡掉的那几次，接口全部误报成"已匹配规则并发出"）。
    uint16_t forwardRecordSerial() const { return nextRecordSeq; }

private:
    IRMapper& mapper;
    IRTransmitter& transmitter;
    MessageQueue& queue;
    Logger& logger;  // 日志管理器引用（用于重置信号间隔）
    bool forwarding;
    bool enabled;  // 转发功能是否启用
    IRGenerator generator;  // 时序生成器
    
    // 时序缓冲区（发射路径「单条时序信号」的编译落点）。
    //
    // 容量取 MAX_LEARN_TIMING_COUNT —— 就是项目声明的"每个指令最大时序点数"。用同一个常量
    // 是为了让"声明上限"与"实际上限"由构造保证一致：学习侧、解析侧、Base64 编码侧全都按它
    // 截断（ir_learner.cpp / unified_signal.cpp），这里再开大只会白占堆。
    //
    // 为什么单条信号的上限是这个数（都是实测/查源得出的，不是估的）：
    //   · 这块缓冲要放下的是**单条信号**，不是整条序列之和 —— sendSequence() 的
    //     "其他协议或序列"分支是逐条 compileSignal() 再 transmit() 的。
    //   · 学习 / 解析字符串：≤200（超限即截断并打警告）。空调遥控的整帧典型就是 100~200 点
    //     ⇒ 评审建议的 100 点会静默截掉后半段波形。
    //   · 协议生成：≤67 —— IRGenerator 只支持 NEC/RC5/RC6/Sony/Samsung，最长是 32 位 = 2+2×32+1。
    //   · 唯一能超过 200 的是**手粘的裸 `timingBase64`**（signalFromJSON 不校验长度）。
    //     ⚠️ v1.3.44 起 useTiming 分支**也走这块缓冲**（原来由 loadTiming() 另开堆）⇒
    //     手粘的超 200 点信号会被 base64ToTiming 截断到 200（现在会打串口日志）。
    //     这是有意的取舍：学习/解析侧本来就按 200 截断，实际信号装得下；换来的是
    //     「学完指令后（空闲堆降到 ~5 KB）仍然发得出去」—— 见 ir_forwarder.cpp 的 useTiming 分支。
    IRTiming timingBuffer[MAX_LEARN_TIMING_COUNT];
    
    // 转发延时相关
    PreciseTimer delayTimer;              // 非阻塞延时定时器
    ForwardDelayState delayState;          // 延时状态
    ForwardTask pendingTask;              // 待转发的任务（延时完成后执行）
    bool pendingTaskValid;                // 待转发任务是否有效
    // 待执行的"时序码 B"任务：延时结束后按 ID 回查规则再发。
    // 为什么不像 NEC 那样入队：时序点阵最多 MAX_LEARN_TIMING_COUNT 点（单条信号），
    // 每条任务要带 4×(2+6) 的码值数组，塞不进 3 深的 ForwardTask 队列（那套结构只存协议码值）。
    // 为什么记 ID 不记指针：延时那 20ms 里 Web 面板可能把规则删了/改了，
    // 裸指针会悬空 —— 回查时找不到就当失败，安全。 -1 = 无。
    int16_t pendingTimingRuleId;
    // 上面那条时序任务对应的转发记录（下标 + 身份号，见 ForwardRecord::seq）
    uint8_t pendingTimingRecordIndex;
    uint16_t pendingTimingRecordSeq;

    // 转发延迟探针的两个时标（见 ForwardRecord::latencyMs）
    unsigned long decodeMs;     // forwardSignal() 入口的 millis()
    unsigned long sendStartMs;  // 发射动作开始的 millis()

    // ── 通用 NEC 遥控器"按住不放"的续发状态 ────────────────────────────
    // 通用遥控器按住时发的是重复码 0xFFFFFFFF，**不带键值**，按 A 码匹配不到任何规则
    // （实测：注入 0xFFFFFFFF 得到 result=1/NOMATCH），于是按住只有第一帧会被转发。
    // 靠"最近命中的规则 + 时间窗"把它续上，见 forwardRepeat()。
    int16_t  lastMatchedRuleId;    // 最近一次命中规则的 ID；-1 = 没有（也用于"换个键"时清掉）
    unsigned long lastMatchedMs;   // 那次命中的时刻
    unsigned long lastRepeatForwardMs;  // 最近一次续发重复码的时刻（0 = 从未）

    // 转发记录环（静态占用 8 × 22 = 176 字节）
    ForwardRecord forwardRecords[MAX_FORWARD_RECORDS];
    uint8_t forwardRecordIndex;           // 下一个写入位置
    uint8_t forwardRecordCount;           // 已写入条数（上限 MAX_FORWARD_RECORDS）
    uint16_t nextRecordSeq;               // 记录身份号发号器（见 ForwardRecord::seq）

    // 写一条记录，返回它的下标；同时把 rec.seq 填上新的身份号
    uint8_t addForwardRecord(const UnifiedSignal& a, int16_t ruleId, uint8_t bCount, uint8_t result);
    // 发射动作起点的打点：记时刻，并把"解码完成 → 开始发射"的毫秒数写进对应记录。
    // ⚠️ 必须在**每次发射之前**调一次；失败路径不调，latencyMs 就保持 0（表示"没走到发射"）。
    void markSendStart(uint8_t recordIndex, uint16_t recordSeq);
    // 发射完成后回填结果/耗时。**按下标回填**，不再回填"最新一条" —— 一条记录在
    // forwardSignal() 里就写了，实际发射可能延后（20ms 延时槽 / 3 深队列），
    // 期间再来信号，"最新一条"就是别人的记录了（2026-10-01 修）。
    void finishForwardRecord(uint8_t recordIndex, uint16_t recordSeq,
                             uint8_t result, uint16_t durationMs);
    // 处理"通用 NEC 遥控器的重复码"：命中窗口内就按最近那条规则续发一次（见上方状态）
    bool forwardRepeat(const UnifiedSignal& aSignal);
    
    bool compileSignal(const UnifiedSignal& signal, IRTiming* timing, int& count);
    bool executeForward(const IR_MapRule& rule);
    void delayPrecise(unsigned long ms);
    
    // 转发相关方法
    void processDelayState();  // 处理延时状态机
    bool createForwardTask(const IR_MapRule& rule, ForwardTask& task);  // 创建转发任务
    bool buildRuleFromTask(const ForwardTask& task, IR_MapRule& rule);  // 从 ForwardTask 恢复规则（统一边界与间隔）
};

#endif // IR_FORWARDER_H

