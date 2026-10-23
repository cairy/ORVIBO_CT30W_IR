#include "ir_parser.h"
#include "config/pins.h"
#include "unified_signal.h"
#include "config/constants.h"
#include "utils/timing_scratch.h"   // v1.3.54：共享时序暂存（省 1600 B .bss）
#include <ArduinoJson.h>

IRParser::IRParser() : irrecv(nullptr) {
}

IRParser::~IRParser() {
    if (irrecv != nullptr) {
        delete irrecv;
        irrecv = nullptr;
    }
}

bool IRParser::init() {
    // 初始化IRremoteESP8266库的接收器
    if (irrecv == nullptr) {
        // ⚠️ 采集缓冲从库默认的 100 项（IRrecv.h:25 kRawBuf）提到 400（v1.3.24）。
        // 为什么要提：实测风扇遥控器的帧 `rawlen` 一路涨到 99→100 并置 overflow=1，
        // 而一条 NEC 帧只要 67 项 —— 100 项 = 200µs 根本装不下，
        // 于是库永远只看到"半个帧"，所有协议匹配必然失败（decode_type 落到 UNKNOWN）。
        //
        // 为什么改构造参数而不是改库：`kRawBuf` 是 IRrecv.h 里的 const，
        // 不是宏，-D 覆盖不了；而 bufsize 是构造函数第 2 个参数
        // （IRrecv.h:124），IRrecv.cpp:299 `params.rawbuf = new uint16_t[bufsize]`
        // 真的按它堆分配，且 kRawBuf 只被当默认值用、全库无别处硬依赖 ⇒ 零库改动。
        //
        // 堆代价：(400-100) × 2 B = 600 B。ESP8266 静态 RAM 还剩 24 KB，
        // 开发机冷态 freeHeap 12.4 KB ⇒ 付得起。
        // 400 项 = 800µs，覆盖库里最长的 A/C 协议（Sharp -Ac 等 100+ 项）还有余量。
        irrecv = new IRrecv(IR_RX_PIN, IR_CAPTURE_BUF_ITEMS);
        if (irrecv == nullptr) {
            return false;
        }
    }

    // 启动接收器
    irrecv->enableIRIn();

    return true;
}

bool IRParser::decodeWithLibrary(UnifiedSignal& signal) {
    if (irrecv == nullptr) {
        return false;
    }

    decode_results results;

    // ⚠️⚠️ 这三个字段**必须先置安全值**（v1.3.24 二次评审揪出的 bug，实机在跑）。
    //   原因：IRrecv::decode() 的第 4 行就是
    //     `if (params.rcvstate != kStopState) return false;`   (IRrecv.cpp:558)
    //   而 rawbuf / rawlen / overflow 的赋值在**它之后**（IRrecv.cpp:581-583）。
    //   ⇒ 只要"接收机正在采集中"，decode() 就**一个字节都不碰 results** 直接返回 false。
    //   而 results 是本函数的**栈局部变量** ⇒ 下面 buildTimingFallback 读到的是栈上残留。
    //   实测：rawlen 残留成 22964 ⇒ count=22963 > 200 ⇒ 每秒刷一行
    //     "[toolong] 帧长 22963 点 > 上限 200"
    //   （串口是同步阻塞的，刷屏本身就是 WDT 复位的成因之一）。
    //   更危险的是残留值若恰好 < 200，那个残留的 rawbuf 指针会被**真的解引用**
    //   ⇒ 越界读堆 ⇒ 凭空冒出一条假信号，甚至崩。
    //   ⛔ 旧注释写"decode() 在**最开始**就填了 rawbuf/rawlen（IRRecv.cpp:578-581）"
    //     是错的 —— 那三行在早退之后，这正是本 bug 的来源。
    results.rawbuf = nullptr;
    results.rawlen = 0;
    results.overflow = false;

    // ⚠️⚠️ 关于 resume() —— 它是必修课，但**只能在"帧真的结束"时调**。
    //   resume() 是 IRrecv 采集状态机**唯一**的复位口（rcvstate=kIdleState、rawlen=0，
    //   IRrecv.cpp:422-424），而 ISR 的第一道闸是
    //     `if (params.rcvstate == kStopState) return;`   (IRrecv.cpp:207)
    //   ⇒ 采集停过一次（超时 / 溢出置 kStopState）后**不 resume 就再也不采任何边沿**，
    //     要等下一次转发触发 main.cpp:425 的 resumeReceiver() 才会醒。
    //   本函数原先只在成功路径（末尾）resume，两条**兜底出口是漏的** ——
    //   而风扇遥控器走的正是兜底 ⇒ 按一次之后后续按键全部收不到。已修。
    //   ⛔ 但反过来同样致命：**采集进行中调 resume() 会把半帧清零**
    //     （rawlen 置 0 + rcvstate 置 kIdleState）。本函数由主循环每
    //     IR_RECEIVER_INTERVAL(10ms) 调一次（main.cpp:415）⇒ 只要在"还在采集"
    //     的分支里 resume()，每一帧都会在半路被抹掉，接收机**永久聋**。
    //     2026-10-03 实机踩过：刷入带这个 bug 的版本后，
    //     按遥控器「最近信号」恒为空、`GET /api/signals/recent?since=0` 返回 `[]`。
    //   ⇒ 判据是下面的 `results.rawlen == 0`，**别省这一步**。
    //   库自己的注释也写着 "This is required if decode() is successful"。

    // 尝试解码
    if (!irrecv->decode(&results)) {
        // ⚠️⚠️ decode() 返回 false 有**两种**原因，处置完全相反（弄错就聋）：
        //
        //   ① **接收机还在采集**（rcvstate != kStopState）⇒ decode() 在第 4 行就早退
        //      （IRrecv.cpp:558），results **一个字节都没被碰过** —— rawlen 仍是上面置的 0。
        //      ⛔ 这种情况**绝不能 resume()**（原因见本函数开头）。正确动作是
        //      **什么都不做、立刻返回**，让采集继续，等下一拍再来。
        //
        //   ② **采集已结束、但库里所有协议都没匹配上** ⇒ decode() 走过了
        //      IRrecv.cpp:581-583，results->rawlen 是**库填的真值**（≥ 1），
        //      原始时序数据完整可用。这时才该落时序码，并且**必须 resume()**。
        //
        // ⭐ 区分 ①② 的唯一判据就是 rawlen（我们置 0  vs  库填真值）。
        //   为什么 kStopState 时 rawlen 必然 ≥ 1：采集定时器**只在 ISR 内启动**
        //   （IRrecv.cpp:229 `os_timer_arm`，在每条边沿记录之后），
        //   而 enableIRIn() 只做 disarm + setfn（不 arm）⇒ kStopState（超时或溢出）
        //   必然发生在至少一条边沿之后 ⇒ rawlen ≥ 1。
        //   ⛔ 别为此去读 IRrecv 的私有全局 `params`（`_ZN7_IRrecv6paramsE` 那种
        //   extern + __asm__ 写法库一升级就链接失败；2026-10-02 为它白绕三轮，
        //   见 TASKS.md 第 21 节）。rawlen 是公开字段，够用。
        if (results.rawlen == 0) return false;   // ① 还在采集：不碰状态机，等下一拍

        // ② 落到这里 = 帧已收完、协议没匹配 ⇒ 用时序码兜底（v1.3.24 新增）。
        //   老固件的手写解析器 parseSignal() 本来就有这条兜底
        //   （ir_parser.cpp:190-199「未匹配协议，使用时序码」），换成库解码后**丢了**，
        //   现在补回来。
        //
        //   实测触发场景：艾美特风扇遥控器（38kHz 载波 + 8 位地址码的 NEC 变体）。
        //   它连发 3~5 遍同一帧（帧长 144/192/216 项），波形规整（帧间 7.2ms），
        //   但库里没有对应解码器 ⇒ 每次都落这里。改用时序码后不依赖协议识别，
        //   匹配/转发/设为 A·B 全部走现成路径（signalsEqual 比 Base64 精确匹配）。
        const bool ok = buildTimingFallback(results, signal);
        irrecv->resume();   // ② 帧已结束 ⇒ 复位采集状态机，准备收下一帧
        return ok;
    }

    // 转换为UnifiedSignal
    bool success = convertDecodeResult(results, signal);

    // 库解出来了、但白名单拒收的信号：同样落成时序码，别让用户白按。
    // 这一支以前是完全静默的（main.cpp 的 if 不成立 ⇒ 不记录不转发），
    // 2026-10-02 就是它让"按了风扇遥控器什么都没发生"。
    if (!success) {
        // 限流：与 [timing] 同理，这行也在热路径上，详见 constants.h 的
        // SERIAL_RATE_LIMIT_MS 注释（串口阻塞曾导致一按遥控器就复位）。
        static unsigned long lastDropPrintMs = 0;
        unsigned long nowMs = millis();
        if (nowMs - lastDropPrintMs >= SERIAL_RATE_LIMIT_MS) {
            lastDropPrintMs = nowMs;
            Serial.printf_P(PSTR("[drop] decode_type=%d 改走时序码\n"), (int)results.decode_type);
        }
        const bool ok = buildTimingFallback(results, signal);
        irrecv->resume();   // ⚠️ 兜底出口也必须复位采集状态机，原因见本函数开头
        return ok;
    }

    // 调试输出（可选，通过宏控制）
    #ifdef DEBUG_IR_DECODE
    Serial.print(F("[IR解码] 协议类型: "));
    Serial.print((int)results.decode_type);
    Serial.print(F(", 值: 0x"));
    Serial.print(results.value, HEX);
    Serial.print(F(", 转换后: "));
    Serial.println(signalToString(signal));
    #endif
    
    // 准备接收下一个信号
    irrecv->resume();
    
    return success;
}

void IRParser::resumeReceiver() {
    // 丢弃当前正在积累的采样、把状态机复位到"等下一个信号"。
    // 发射期间与发射后的静默期里，接收头看到的是我们自己发出的光，
    // 这些采样必须丢掉 —— 否则发射结束后会解出一个半帧垃圾。
    if (irrecv != nullptr) {
        irrecv->resume();
    }
}

void IRParser::disableLibraryReceiver() {
    if (irrecv != nullptr) {
        // 禁用中断处理程序，重新注册自定义中断处理程序
        detachInterrupt(digitalPinToInterrupt(IR_RX_PIN));
    }
}

void IRParser::enableLibraryReceiver() {
    if (irrecv != nullptr) {
        // 重新启用库的接收器
        irrecv->enableIRIn();
    }
}

bool IRParser::convertDecodeResult(decode_results& results, UnifiedSignal& signal) {
    // 初始化UnifiedSignal
    signal.clear();

    // 检查是否有有效数据
    // IRremoteESP8266库中，UNKNOWN通常值为-1
    if (results.decode_type == UNKNOWN) {
        // 未知协议，使用时序码（但这里没有时序数据，所以返回false）
        // 实际使用时序码的情况应该在parseSignal中处理
        signal.useTiming = false;  // 既不是协议也不是时序码（无效状态）
        return false;
    }

    // 已知协议，转换为我们的协议编号
    signal.useTiming = false;  // 协议信号
    signal.data = (uint32_t)results.value;
    signal.repeatCount = 1;

    // 协议类型映射（使用库的枚举值）
    // IRremoteESP8266库使用decode_type_t枚举，直接比较枚举值
    decode_type_t decodeType = results.decode_type;

    // ⚠️ 协议编号映射：必须与手写解析器（parseNEC/RC5/...）保持一致
    // 根据协议类型映射到我们的协议编号
    // 只使用库中确实存在的枚举值
    //
    // ⭐ Epson 并进 NEC（v1.3.24，实测加的）
    // 实测证据（开发机 COM4，风扇遥控器 4 键，2026-10-02）：
    //   采集缓冲提到 400 项后不再溢出，帧长 144 / 192 / 216 项；
    //   波形是教科书式 NEC —— 引导码 7228/8096/8136/8114/8130 µs
    //   （NEC 要求 9000，容差 25% ⇒ 允许 6750~11250），数据位三档
    //   1290~1354（bit1 mark 1680）/ 1236~1272（bit0 mark 1200）/ 386~490（space 560），
    //   逐位严格 mark/space 交替。
    // 为什么库报 Epson 而不是 NEC：两者位宽一致，而 IRrecv.cpp:644-648
    // 明确"先试 decodeEpson() 再 decodeNEC()，因为它们无法区分" ⇒ Epson 先命中。
    // ⇒ 归到 protocol=1 后，转发走 sendNEC() 是对的（位宽兼容）。
    //
    // ⚠️⚠️ 数枚举表时注意：`UNKNOWN = -1` 是**显式赋值**（IRremoteESP8266.h:1013），
    // 所以第一项不是 0。若按"第 0 项 = UNKNOWN"去数，后面全部偏移 1 ——
    // 我因此一度以为 EPSON=76，实测是 75（2026-10-02 设备打印自证）。
    // ⇒ 引用协议一律用库给的枚举**名字**，不要自己数索引。
    //
    // ⚠️ Epson 解码器要求**同一条数据连发两遍**（ir_Epson.cpp 的
    // kEpsonMinMesgsForDecode=2，且 data != first_data 时 return false），
    // 风扇遥控器单次按键发 3~5 遍（实测帧长 144/192/216 = 48×3/4/5），
    // 所以 Epson 时而命中、时而 UNKNOWN。
    // ⚠️ 别顺手把其它"时序相近"的协议也加进来：每个都要实测波形确认过才加，
    // 没证据的映射会导致解出来的 data 其实是另一种协议的值，转发出去是错的。
    if (decodeType == NEC || decodeType == NEC_LIKE || decodeType == EPSON) {
        signal.protocol = 1;  // NEC
    }
    else if (decodeType == RC5 || decodeType == RC5X) {
        signal.protocol = 2;  // RC5
    }
    else if (decodeType == RC6) {
        signal.protocol = 3;  // RC6
    }
    else if (decodeType == SONY) {
        signal.protocol = 4;  // Sony
    }
    else if (decodeType == SAMSUNG || decodeType == SAMSUNG36) {
        signal.protocol = 5;  // Samsung
    }
    // 其他协议，不支持
    else {
        signal.useTiming = false;  // 无效状态
        signal.protocol = 0;
        return false;
    }

    return true;
}

// 两段采集切片是否"同一条信号"：点数相同 + 逐点容差（判据与 SignalUtils::signalsEqual
// 对时序码的口径一致，见 constants.h 的 TIMING_MATCH_*）。
// ⚠️ 直接在 rawbuf 的 tick 值上比，不经过 Base64 —— 这里只用来**判断是不是重复结构**，
//    真正的编码仍然只走 timingToBase64 一处。
// buf 是 results.rawbuf（tick，1 tick = 2µs）；s1/s2 是段起点（在 rawbuf[1..] 里的下标）。
static bool segmentsTolerantlyEqual(const volatile uint16_t* buf,
                                    int s1, int s2, int len) {
    if (buf == nullptr || len <= 0) return false;
    for (int i = 0; i < len; i++) {
        const unsigned long x = (unsigned long)buf[s1 + i + 1] * 2UL;
        const unsigned long y = (unsigned long)buf[s2 + i + 1] * 2UL;
        const unsigned long diff = (x > y) ? (x - y) : (y - x);
        unsigned long tol = ((x > y) ? x : y) * TIMING_MATCH_REL_PERCENT / 100UL;
        if (tol < TIMING_MATCH_ABS_TOL_US) tol = TIMING_MATCH_ABS_TOL_US;
        if (diff > tol) return false;
    }
    return true;
}

bool IRParser::buildTimingFallback(const decode_results& results, UnifiedSignal& signal) {
    signal.clear();

    // rawbuf[0] 是库在 kIdleState 写下的一格高电平标记（IRrecv.cpp:212-213
    // 写的是常量 1，不是真实时长）⇒ 必须跳过，否则每次都多一个 2µs 的假脉冲，
    // 而时序码是按**点数 + 逐点时长**比对的（signalsEqual，v1.3.25 起带容差），
    // 多一格就直接判成不同的信号。真实的 mark/space 从 rawbuf[1] 开始，
    // 与 base64ToTiming 的 level=true 起点一致。
    if (results.rawlen < 2 || results.rawbuf == nullptr) {
        return false;   // 只有起点标记 = 什么都没采到
    }

    int count = (int)results.rawlen - 1;

    // ⚠️ 先剥掉尾部时长为 0 的假点：采集收尾会多带一格 0µs（实测学习模式取到的那段
    //   是 24 点、最后一点 tick=0）。留着它会让"最后一段"比别的段长 1 点、
    //   从而破坏下面的重复性判据。0µs 的脉冲物理上不存在，剥掉一定安全。
    while (count > 0 && results.rawbuf[count] == 0) {
        count--;
    }

    // ⚠️ 短于 20 点的一律当噪声：环境里的 2kHz 方波（实测 rawlen 6~18）就是这种，
    // 收进来只会占堆。库里 isNoise 的门槛是 50µs，这里用点数判更直观。
    if (count < 20) {
        // 限流：环境里的 2kHz 方波持续存在（实测 rawlen 6~18），
        // 不限流每 10ms 打一行，串口阻塞会拖死主循环（同上）。
        static unsigned long lastNoisePrintMs = 0;
        unsigned long nowMs = millis();
        if (nowMs - lastNoisePrintMs >= SERIAL_RATE_LIMIT_MS) {
            lastNoisePrintMs = nowMs;
            Serial.printf_P(PSTR("[noise] rawlen=%u 太短，丢弃\n"), (unsigned)results.rawlen);
        }
        return false;
    }

    // ⚠️⚠️ 按帧间长间隔把"连发的多帧"切成"一帧"（v1.3.25）。
    //
    //   实测（2026-10-03，学习模式取原始波形）：遥控器按一次连发 7~12 遍同一帧，
    //   遍与遍之间只有 7,270µs 静默，而接收机判停要 15,000µs
    //   ⇒ 一次按键在缓冲里是**一整段**，长度随"从哪条边沿起采"和"收到几遍"而变：
    //   实测 170 / 191 / 215 / 239 / 287 点，全都不一样。
    //
    //   后果两条，都致命：
    //     ① > MAX_LEARN_TIMING_COUNT(200) 的整段被丢弃 —— 215/239/287 全没了；
    //     ② 侥幸 ≤200 的，每次长度不同 ⇒ Base64 不同 ⇒ 用户"设为 A"之后再按同一个键，
    //        signalsEqual 也认不出来 ⇒ **永远不转发**。
    //   ⇒ 判据不是"放开上限"，而是"切成单帧再存"。
    //
    //   ⭐⭐ 切法必须**自证是重复结构**，不能"见长间隔就切"：
    //   实测 NEC 帧头是 9000µs mark + **4500µs space**，比本阈值(3500)还长 ——
    //   裸切会把整个帧头剥掉（模拟验证：NEC 35 点被切成 33 点，前两点没了，
    //   而 9000/4500 正是接收端识别 NEC 的判据）。所以判据是：
    //     **一段必须"出现 ≥2 次、且两段内容容差相同"才认得下它**。
    //   · 风扇：7 帧逐点相同（只有 ±8% 占空比漂移）⇒ 认得出 ⇒ 切成 23 点 ✓
    //   · NEC 单帧：只有帧头那一个长间隔，切出的两段一个 1 点、一个 33 点，
    //     没有等长且内容相同的第二段 ⇒ **不切**，整段照旧 ✓
    //   · 本就不带长间隔的信号 ⇒ 不切，与旧行为完全一致 ✓
    //   ⇒ 这次改动只会**多收**信号，不会少收，也不会把已知协议切坏。
    int segStart = 0;
    int segLen   = count;
    {
        int st[MAX_BURST_SEGMENTS], ln[MAX_BURST_SEGMENTS], n = 0;
        int cur = 0;
        for (int i = 0; i < count; i++) {
            // 长间隔只可能出现在 SPACE 位置（偶数下标 = MARK 高电平，奇数 = SPACE）
            if ((i % 2) == 1 &&
                (unsigned long)results.rawbuf[i + 1] * 2UL >= FRAME_GAP_MIN_US) {
                if (n < MAX_BURST_SEGMENTS) { st[n] = cur; ln[n] = i - cur; n++; }
                cur = i + 1;                 // 下一段从长间隔之后那条边沿起
            }
        }
        if (n < MAX_BURST_SEGMENTS) { st[n] = cur; ln[n] = count - cur; n++; }

        int pickSt = -1, pickLn = 0;
        for (int i = 0; i < n; i++) {
            if (ln[i] < MIN_TIMING_SEGMENT_POINTS || ln[i] <= pickLn) continue;
            for (int j = i + 1; j < n; j++) {
                if (ln[j] != ln[i]) continue;
                if (segmentsTolerantlyEqual(results.rawbuf, st[i], st[j], ln[i])) {
                    pickSt = st[i];
                    pickLn = ln[i];
                    break;
                }
            }
        }
        // ⭐ 再排除"帧头自带长间隔"的协议（v1.3.25）。
        //   例：NEC 帧头 9000µs mark + 4500µs space，4500 > 阈值 ⇒ 也切一刀，
        //   于是段长变成 [1, 33, 1, 33]（连发 2 帧）—— 两组 33 点容差相同，
        //   会被误认为"重复单元"，**把 9000/4500 帧头整个剥掉**（模拟已复现）。
        //   而真正的帧间间隔只会在整段的**头尾**留下残片
        //   （风扇实测段长 [1, 23, 23, 23, 23, 23, 23, 23]：短的只有开头那一段）。
        //   判据：选中长度之外的段，只允许出现在下标 0 与 n-1。
        if (pickLn > 0) {
            for (int i = 0; i < n; i++) {
                if (ln[i] == pickLn) continue;
                if (i != 0 && i != n - 1) { pickSt = -1; pickLn = 0; break; }
            }
        }
        if (pickLn > 0) {
            segStart = pickSt;
            segLen   = pickLn;
        }
    }

    // ⭐ 切段**不改变电平奇偶**：长间隔只出现在奇数下标，所以每段起点恒为偶数。
    //   ⇒ 每一段都从 MARK 开始、到 MARK 结束，下面 `i % 2 == 0` 判 level 依然成立。
    //   ⚠️ 别把 segStart 丢掉 —— 它必须以 offset 的形式进到取值循环里。
    const int offset = segStart;
    count = segLen;

    // ⚠️⚠️ 超长帧**丢弃**，不静默截断（v1.3.24 审查时改的）。
    // MAX_LEARN_TIMING_COUNT=200 是 hard 上限：timingToBase64 的栈缓冲只有 400 B
    // （unified_signal.cpp:150），超过 200 点它直接返回空串。
    // 原来写的是"截到 200 点"⇒ 实测 216 点的帧被砍掉 16 点，
    // 落成一条**不完整**的时序码 ⇒ 用户拿它"设为 A"或转发，被控设备**不会响应**，
    // 而且界面上看不出任何异常（Base64 字符串是合法的，只是少了尾巴）。
    // 宁可少一条记录，也不能给一条"看着正常、实际不能用"的。
    //
    // 提高上限需要同时放大 timingToBase64 的栈缓冲（200→350 点即 700 B）
    // 并重算全链路堆预算（MQTT 入站上界、入站长度闸 768）—— 那是独立的改动，
    // 现在先按"丢弃 + 明确告警"处理，见 logger.cpp 串口输出处的提示。
    if (count > MAX_LEARN_TIMING_COUNT) {
        static unsigned long lastTooLongPrintMs = 0;
        unsigned long nowMs = millis();
        if (nowMs - lastTooLongPrintMs >= SERIAL_RATE_LIMIT_MS) {
            lastTooLongPrintMs = nowMs;
            Serial.printf_P(PSTR("[toolong] 帧长 %d 点 > 上限 %d（原采 %d 点，已切到单帧仍未装下），丢弃\n"),
                          count, MAX_LEARN_TIMING_COUNT, (int)results.rawlen - 1);
        }
        return false;
    }

    // ⚠️⚠️ 这里用 **static** 局部数组，不是栈数组（v1.3.24 修）：
    // 栈上 200 × sizeof(IRTiming) = 200 × 8 B = **1.6 KB**，
    // 而 timingToBase64 内部还有 400 B 栈缓冲（unified_signal.cpp:154）。
    // 两者叠加在 decodeWithLibrary → buildTimingFallback → timingToBase64
    // 这条调用链上，最深处还叠着 decode_results / SignalSequence 等帧 ——
    // ESP8266 的任务栈本就紧张，热路径上再压 2 KB 是**栈溢出**的典型成因。
    //
    // ⭐ v1.3.54：改接**全局共享 scratch**（`timing_scratch.h`）。
    // 本文件是当初"把多处用完即弃的 IRTiming 暂存合共用一块"时**漏掉的第 4 处**，
    // 它自己那份 static 数组独占 200 × 8 B = **1600 B 的 .bss**，
    // 而本机每 1 B .bss 就是 1 B 可用堆 ⇒ 白占 1.6 KB。
    // 安全性同原注释 —— buildTimingFallback **不可重入**：
    // 唯一调用点是 decodeWithLibrary，主循环单线程调用，无并发/递归；
    // 且本函数返回前数据已由 timingToBase64() 编码进 signal.timingBase64（深拷贝 String），
    // 返回后不再引用这块缓冲。timingScratch() 的其他使用者（学习接收 / 发射解析 / 规则解析）
    // 与接收解码路径不重叠（发射是阻塞的，期间收不到新码）。
    //
    // ⚠️ 仍不能改回**栈数组**：200 × 8 B = 1.6 KB 栈 + timingToBase64 内部 400 B 栈，
    // 叠在 decodeWithLibrary → buildTimingFallback → timingToBase64 链上会栈溢出。
    IRTiming* const timing = timingScratch();

    for (int i = 0; i < count; i++) {
        // kRawTick = 2（IRrecv.h:37）⇒ tick 数 × 2 = 微秒
        // ⚠️ offset 是切段后的起点（切段保证为偶数），漏掉它会把上一帧的尾巴收进来
        timing[i].duration = (unsigned long)results.rawbuf[offset + i + 1] * 2;
        // rawbuf[1] 是第一个真实脉冲 = 载波 mark = 高电平，之后逐项交替
        timing[i].level = ((offset + i) % 2 == 0);
    }

    // ⚠️ 这里**不**复用 parseSignal()：它会先试一遍手写 NEC/RC5/RC6/Sony/Samsung
    // 五种协议，对我们"库都认不出"的信号必然全部失败，纯浪费 CPU。
    // Base64 编码仍然只走 timingToBase64 一处（判据：编码只有一份实现）。
    signal.useTiming = true;
    signal.protocol = 0;
    signal.data = 0;
    signal.timingCount = count;
    signal.timingBase64 = SignalUtils::timingToBase64(timing, count);
    // ⚠️ repeatCount 不能留 1（v1.3.25 改）：遥控器自己一次按键连发 7~12 遍同一帧
    //   （实测），我们只存了其中**一遍**。只发一遍，被控设备很可能不响应
    //   （多数红外接收端要"连续几遍一致"才认，重复是抗噪声用的）。
    //   v1.3.26 起取 **7**：与遥控器实测的突发长度一致，且帧间隔交给
    //   TIMING_BURST_GAP_US(7,270µs) 复现 —— 两者合起来才是"原来那串突发"
    //   （v1.3.25 只发 3 遍、间隔 100ms，22.6 倍的时间尺度差，收方不认）。
    //   上限由 SIGNAL_REPEAT_MAX(10) 在 sendSequence 里兜着。
    signal.repeatCount = 7;

    if (signal.timingBase64.length() == 0) {
        signal.clear();
        // 限流，同上（这条在帧长 > 200 点时才会走到，频率低，但仍在热路径）
        static unsigned long lastEncFailPrintMs = 0;
        unsigned long nowMs = millis();
        if (nowMs - lastEncFailPrintMs >= SERIAL_RATE_LIMIT_MS) {
            lastEncFailPrintMs = nowMs;
            Serial.printf_P(PSTR("[noise] rawlen=%u Base64 编码失败，丢弃\n"), (unsigned)results.rawlen);
        }
        return false;
    }

    // ⚠️ 串口限流（v1.3.24）：这行原本每次都打，而一次按键会连发 3~5 遍
    // ⇒ 每遍一行 ≈ 6 ms 阻塞（115200 波特）。与 Logger 的 [recv] 行叠加后
    // 单帧超过 17 ms > IR_RECEIVER_INTERVAL(10ms) ⇒ loop() 喂不到 WDT、复位。
    // 实测一次按键打出 11214 行后设备重启。详见 logger.cpp 同处注释。
    static unsigned long lastTimingPrintMs = 0;
    unsigned long nowMs = millis();
    if (nowMs - lastTimingPrintMs >= 1000) {
        lastTimingPrintMs = nowMs;
        Serial.printf_P(PSTR("[timing] 库未识别，落时序码 %d 点 / Base64 %u 字符（原采 %d 点，切到单帧）\n"),
                  (int)signal.timingCount, (unsigned)signal.timingBase64.length(),
                  (int)results.rawlen - 1);
    }
    return true;
}

bool IRParser::parseSignal(const IRTiming* timing, int count, UnifiedSignal& signal) {
    if (timing == nullptr || count <= 0) {
        return false;
    }

    // 过滤噪声（<50μs的脉冲）
    int validCount = 0;
    for (int i = 0; i < count; i++) {
        if (!isNoise(timing[i].duration)) {
            validCount++;
        }
    }

    if (validCount < 10) {
        return false;  // 信号太短，可能是噪声
    }

    // 限制时序码数量
    int maxCount = (count > MAX_LEARN_TIMING_COUNT) ? MAX_LEARN_TIMING_COUNT : count;

    // ⚠️ 手写解析器：协议编号必须与 convertDecodeResult 保持一致
    // 尝试解析各种协议
    UnifiedSignal tempSignal;
    if (parseNEC(timing, maxCount, tempSignal)) {
        signal = tempSignal;
        signal.useTiming = false;  // 协议信号
        signal.repeatCount = 1;
        return true;
    }
    if (parseRC5(timing, maxCount, tempSignal)) {
        signal = tempSignal;
        signal.useTiming = false;  // 协议信号
        signal.repeatCount = 1;
        return true;
    }
    if (parseRC6(timing, maxCount, tempSignal)) {
        signal = tempSignal;
        signal.useTiming = false;  // 协议信号
        signal.repeatCount = 1;
        return true;
    }
    if (parseSony(timing, maxCount, tempSignal)) {
        signal = tempSignal;
        signal.useTiming = false;  // 协议信号
        signal.repeatCount = 1;
        return true;
    }
    if (parseSamsung(timing, maxCount, tempSignal)) {
        signal = tempSignal;
        signal.useTiming = false;  // 协议信号
        signal.repeatCount = 1;
        return true;
    }

    // 未匹配协议，使用时序码
    signal.clear();
    signal.useTiming = true;  // 时序码信号
    signal.protocol = 0;
    signal.data = 0;
    signal.timingCount = maxCount;
    signal.timingBase64 = SignalUtils::timingToBase64(timing, maxCount);
    signal.repeatCount = 1;

    return signal.timingBase64.length() > 0;
}

String IRParser::getProtocolName(uint32_t protocol) const {
    // 协议名称映射
    switch (protocol) {
        case 1: return "NEC";
        case 2: return "RC5";
        case 3: return "RC6";
        case 4: return "Sony";
        case 5: return "Samsung";
        default: return "Unknown";
    }
}

String IRParser::signalToString(const UnifiedSignal& signal) const {
    if (signal.useTiming) {
        return "Timing:" + String(signal.timingCount) + " points";
    } else if (signal.isProtocol()) {
        return getProtocolName(signal.protocol) + ":" + String(signal.data, HEX);
    } else {
        return "Unknown";
    }
}

bool IRParser::parseNEC(const IRTiming* timing, int count, UnifiedSignal& signal) {
    // NEC协议解析
    // NEC协议格式：引导码(9ms高+4.5ms低) + 32位数据
    // 标准格式：地址(8位) + 地址反码(8位) + 命令(8位) + 命令反码(8位)
    // 扩展格式：地址(16位) + 命令(16位)
    // 重复码：9ms高 + 2.25ms低 + 560μs低（无数据）
    
    if (count < 10) {
        return false;  // 至少需要引导码
    }
    
    int idx = 0;
    
    // 检查引导码：第一个应该是高电平，持续时间约9ms（容差±20%）
    // 注意：实际捕获的时序数据可能从低电平开始（空闲状态）
    // 需要跳过初始的低电平或高电平
    while (idx < count && timing[idx].duration < 100) {
        idx++;  // 跳过很短的初始时序（可能是噪声）
    }
    
    if (idx >= count) {
        return false;
    }
    
    // 如果第一个有效时序是低电平，跳过它（可能是空闲状态）
    if (!timing[idx].level) {
        idx++;
        if (idx >= count) {
            return false;
        }
    }
    
    // 检查引导码：标准NEC是9ms高电平，但实际捕获可能是4.5ms
    // 如果第一个高电平是4.5ms，且下一个低电平是560μs左右（数据位），
    // 说明引导码的9ms高电平部分可能被省略了，直接开始解析数据
    bool headerFound = false;
    
    if (timing[idx].level && timing[idx].duration >= 7200 && timing[idx].duration <= 10800) {
        // 标准9ms引导码
        headerFound = true;
        idx++;
        if (idx >= count) return false;
    } else if (timing[idx].level && timing[idx].duration >= 3600 && timing[idx].duration <= 5400) {
        // 可能是4.5ms高电平（非标准格式，或者时序数据从中间开始）
        // 检查下一个低电平
        idx++;
        if (idx >= count) return false;
        
        if (!timing[idx].level && timing[idx].duration >= 3600 && timing[idx].duration <= 5400) {
            // 下一个低电平也是4.5ms，这是引导码的低电平部分
            headerFound = true;
            idx++;
        } else if (!timing[idx].level && timing[idx].duration >= 448 && timing[idx].duration <= 672) {
            // 下一个低电平是560μs左右（数据位），说明引导码的低电平部分被省略了
            // 这种情况下，4.5ms高电平可能是引导码的一部分，直接开始解析数据
            headerFound = true;
            // idx已经指向数据位的低电平部分，不需要回退
            // 数据位从当前的低电平开始解析
        } else {
            return false;
        }
    } else {
        return false;
    }
    
    if (!headerFound) {
        return false;
    }
    
    if (idx >= count) return false;
    
    // 检查是否为重复码（引导码后低电平约2.25ms）
    if (!timing[idx].level && timing[idx].duration >= 1800 && timing[idx].duration <= 2700) {
        // 重复码：9ms高 + 2.25ms低 + 560μs低
        idx++;
        if (idx >= count) return false;
        if (timing[idx].level || timing[idx].duration < 400 || timing[idx].duration > 700) {
            return false;
        }
        // 重复码使用上一次的数据（这里返回false，由上层处理）
        return false;
    }
    
    // 如果处理的是9ms高电平，需要检查引导码的低电平部分（4.5ms）
    // 如果处理的是4.5ms高+4.5ms低，已经处理完成，直接开始解析数据
    // 如果处理的是4.5ms高+数据位低，需要检查是否已经跳过4.5ms低电平
    bool needCheckLow = false;
    if (idx > 0 && timing[idx-1].duration >= 7200 && timing[idx-1].duration <= 10800) {
        // 上一个时序点是9ms高电平，需要检查4.5ms低电平
        needCheckLow = true;
    }
    
    if (needCheckLow) {
        // 检查引导码的低电平部分，约4.5ms（容差±20%）
        if (timing[idx].level || timing[idx].duration < 3600 || timing[idx].duration > 5400) {
            return false;
        }
        idx++;
    }
    
    // 解析32位数据
    if (count < idx + 64) {
        return false;  // 至少需要64个时序点（32位*2）
    }
    
    uint32_t data = 0;
    for (int bit = 0; bit < 32; bit++) {
        if (idx >= count - 1) {
            return false;  // 数据不足
        }
        
        // 每个位由560μs低电平 + 载波时间组成
        // 逻辑0: 560μs载波（容差±20%）
        // 逻辑1: 1680μs载波（容差±20%）
        if (timing[idx].level) {
            return false;  // 应该是低电平
        }
        
        // 放宽容差范围到±20%
        if (timing[idx].duration < 448 || timing[idx].duration > 672) {
            return false;  // 低电平持续时间应该在560μs左右
        }
        idx++;
        
        if (idx >= count) {
            return false;
        }
        
        // 检查载波时间判断是0还是1
        if (!timing[idx].level) {
            return false;  // 应该是高电平（载波）
        }
        
        unsigned long carrierTime = timing[idx].duration;
        // 放宽容差范围到±20%
        if (carrierTime > 1344) {  // 1680 * 0.8 = 1344
            // 逻辑1：载波时间约1680μs
            // ⚠️ 位序（v1.3.58）：这里采集到的第 bit 位是**时间上先到**的位，而发射/接收走库的
            //   sendNEC/decodeNEC，按 MSB→LSB 处理（ir_NEC.cpp `MSBfirst = true`）
            //   ⇒ 必须填到 bit31-bit，才与发射、实时接收同一口径。
            //   填成 bit（旧写法）会让"学到的码直接发出去"整串逐位反序
            //   （2026-10-06 闭环实测：212 发 0xBC43EE11 → 111 采集回 0x8877C23D）。
            //   RC5/RC6/Sony 本来就是"从高位开始"（本文件 801/888/983 行），无需改。
            data |= (1UL << (31 - bit));
        } else if (carrierTime > 448) {  // 560 * 0.8 = 448
            // 逻辑0：载波时间约560μs
            // data位保持0
        } else {
            return false;  // 载波时间异常
        }
        idx++;
    }
    
    // NEC协议使用32位数据：地址(8位) + 地址反码(8位) + 命令(8位) + 命令反码(8位)
    // 或者扩展格式：地址(16位) + 命令(16位)
    // 这里我们使用完整的32位数据作为信号标识
    signal.protocol = 1;  // NEC
    signal.data = data;  // 使用完整的32位数据
    signal.useTiming = false;  // 协议信号
    return true;
}

bool IRParser::parseRC5(const IRTiming* timing, int count, UnifiedSignal& signal) {
    // RC5协议解析
    // 载波频率：36kHz
    // 位时间：1.778ms（64个载波周期）
    // 曼彻斯特编码：位中间跳变表示逻辑值
    // 数据格式：1位起始位(2个位时间) + 1位控制位 + 5位地址 + 6位命令（共13位）
    
    if (count < 30) return false;  // RC5至少需要30个时序点（13位*2 + 起始位）
    
    int idx = 0;
    
    // 检测起始位：2个位时间（约3.556ms）
    // 起始位应该是高电平或低电平，持续2个位时间
    unsigned long startBitDuration = 0;
    bool startLevel = timing[idx].level;
    
    // 跳过起始位的前半部分
    while (idx < count && timing[idx].level == startLevel) {
        startBitDuration += timing[idx].duration;
        idx++;
    }
    
    // 起始位应该是2个位时间（1.778ms * 2 = 3.556ms，容差±20%）
    if (startBitDuration < 2845 || startBitDuration > 4267) {
        return false;
    }
    
    // 解析13位数据（1位控制位 + 5位地址 + 6位命令）
    uint32_t data = 0;
    
    for (int bit = 0; bit < 13; bit++) {
        if (idx >= count - 1) {
            return false;
        }
        
        // 曼彻斯特编码：位中间必须有跳变
        // 逻辑0：前半部分低电平，后半部分高电平（或相反）
        // 逻辑1：前半部分高电平，后半部分低电平（或相反）
        
        unsigned long firstHalf = 0;
        unsigned long secondHalf = 0;
        bool firstLevel = timing[idx].level;
        
        // 读取前半部分
        while (idx < count && timing[idx].level == firstLevel) {
            firstHalf += timing[idx].duration;
            idx++;
        }
        
        if (idx >= count) return false;
        
        // 读取后半部分
        bool secondLevel = timing[idx].level;
        while (idx < count && timing[idx].level == secondLevel) {
            secondHalf += timing[idx].duration;
            idx++;
        }
        
        // 检查是否有跳变（前后电平应该不同）
        if (firstLevel == secondLevel) {
            return false;  // 曼彻斯特编码必须有跳变
        }
        
        // 检查位时间（容差±20%）
        unsigned long totalTime = firstHalf + secondHalf;
        if (totalTime < 1422 || totalTime > 2134) {
            return false;
        }
        
        // 判断逻辑值：根据跳变方向
        // 如果前半部分时间更长，可能是逻辑0或1，需要根据实际协议判断
        // RC5协议：如果前半部分是低电平，则为逻辑0；如果前半部分是高电平，则为逻辑1
        if (firstLevel == false) {  // 前半部分是低电平
            // 逻辑0
            // data位保持0
        } else {  // 前半部分是高电平
            // 逻辑1
            data |= (1UL << (12 - bit));  // 从高位开始
        }
    }
    
    signal.protocol = 2;  // RC5
    signal.data = data;
    signal.useTiming = false;  // 协议信号
    return true;
}

bool IRParser::parseRC6(const IRTiming* timing, int count, UnifiedSignal& signal) {
    // RC6协议解析
    // 载波频率：36kHz
    // 前导码：6ms高 + 2ms低 + 266μs高 + 266μs低
    // 位时间：444μs（16个载波周期）
    // 数据格式：前导码 + 模式位 + 20位数据
    
    if (count < 50) return false;  // RC6至少需要50个时序点
    
    int idx = 0;
    
    // 检测前导码：6ms高 + 2ms低 + 266μs高 + 266μs低（容差±20%）
    // 6ms高电平
    if (!timing[idx].level || timing[idx].duration < 4800 || timing[idx].duration > 7200) {
        return false;
    }
    idx++;
    
    if (idx >= count) return false;
    
    // 2ms低电平
    if (timing[idx].level || timing[idx].duration < 1600 || timing[idx].duration > 2400) {
        return false;
    }
    idx++;
    
    if (idx >= count) return false;
    
    // 266μs高电平
    if (!timing[idx].level || timing[idx].duration < 213 || timing[idx].duration > 319) {
        return false;
    }
    idx++;
    
    if (idx >= count) return false;
    
    // 266μs低电平
    if (timing[idx].level || timing[idx].duration < 213 || timing[idx].duration > 319) {
        return false;
    }
    idx++;
    
    // 解析模式位（1位，444μs）
    if (idx >= count - 1) return false;
    
    unsigned long modeBitTime = 0;
    bool modeLevel = timing[idx].level;
    while (idx < count && timing[idx].level == modeLevel) {
        modeBitTime += timing[idx].duration;
        idx++;
    }
    
    // 模式位时间应该是444μs（容差±20%）
    if (modeBitTime < 355 || modeBitTime > 533) {
        return false;
    }
    
    // 解析20位数据
    uint32_t data = 0;
    
    for (int bit = 0; bit < 20; bit++) {
        if (idx >= count) {
            return false;
        }
        
        // RC6使用脉宽编码：短脉冲(444μs)表示0，长脉冲(888μs)表示1
        unsigned long pulseTime = 0;
        bool pulseLevel = timing[idx].level;
        
        while (idx < count && timing[idx].level == pulseLevel) {
            pulseTime += timing[idx].duration;
            idx++;
        }
        
        // 判断逻辑值
        if (pulseTime >= 710) {  // 888 * 0.8 = 710
            // 长脉冲：逻辑1
            data |= (1UL << (19 - bit));  // 从高位开始
        } else if (pulseTime >= 355) {  // 444 * 0.8 = 355
            // 短脉冲：逻辑0
            // data位保持0
        } else {
            return false;  // 脉冲时间异常
        }
        
        // 跳过低电平间隔
        if (idx < count && !timing[idx].level) {
            idx++;
        }
    }
    
    signal.protocol = 3;  // RC6
    signal.data = data;
    signal.useTiming = false;  // 协议信号
    return true;
}

bool IRParser::parseSony(const IRTiming* timing, int count, UnifiedSignal& signal) {
    // Sony协议解析
    // 载波频率：40kHz
    // 引导码：2400μs高 + 600μs低（12位）或4800μs高 + 600μs低（15/20位）
    // 数据位：1200μs高 + 600μs低（1）或600μs高 + 600μs低（0）
    // 数据格式：12/15/20位
    
    if (count < 20) return false;
    
    int idx = 0;
    
    // 检测引导码，确定数据位数
    if (!timing[idx].level) {
        return false;
    }
    
    unsigned long headerHigh = timing[idx].duration;
    idx++;
    
    if (idx >= count) return false;
    
    if (timing[idx].level || timing[idx].duration < 480 || timing[idx].duration > 720) {
        return false;  // 引导码低电平应该是600μs（容差±20%）
    }
    idx++;
    
    int dataBits = 0;
    // 根据引导码高电平时间判断数据位数
    if (headerHigh >= 1920 && headerHigh <= 2880) {  // 2400 * 0.8 = 1920, 2400 * 1.2 = 2880
        dataBits = 12;
    } else if (headerHigh >= 3840 && headerHigh <= 5760) {  // 4800 * 0.8 = 3840, 4800 * 1.2 = 5760
        // 需要进一步判断是15位还是20位
        // 根据数据位数量判断（15位需要约30个时序点，20位需要约40个时序点）
        int remainingPoints = count - idx;
        if (remainingPoints >= 35) {
            dataBits = 20;
        } else {
            dataBits = 15;
        }
    } else {
        return false;  // 引导码时间不符合Sony协议
    }
    
    // 解析数据位
    uint32_t data = 0;
    
    for (int bit = 0; bit < dataBits; bit++) {
        if (idx >= count - 1) {
            return false;
        }
        
        // 数据位：1200μs高 + 600μs低（1）或600μs高 + 600μs低（0）
        bool highLevel = timing[idx].level;
        unsigned long highTime = 0;
        
        while (idx < count && timing[idx].level == highLevel) {
            highTime += timing[idx].duration;
            idx++;
        }
        
        if (idx >= count) return false;
        
        // 低电平部分（600μs）
        unsigned long lowTime = 0;
        bool lowLevel = timing[idx].level;
        while (idx < count && timing[idx].level == lowLevel) {
            lowTime += timing[idx].duration;
            idx++;
        }
        
        // 判断逻辑值：根据高电平时间
        // 逻辑1: 1200μs高（容差±20% = 960-1440μs）
        // 逻辑0: 600μs高（容差±20% = 480-720μs）
        if (highTime >= 960) {  // 1200 * 0.8 = 960
            // 逻辑1
            data |= (1UL << (dataBits - 1 - bit));  // 从高位开始
        } else if (highTime >= 480) {  // 600 * 0.8 = 480
            // 逻辑0
            // data位保持0
        } else {
            return false;  // 高电平时间异常
        }
    }
    
    signal.protocol = 4;  // Sony
    signal.data = data;
    signal.useTiming = false;  // 协议信号
    return true;
}

bool IRParser::parseSamsung(const IRTiming* timing, int count, UnifiedSignal& signal) {
    // Samsung协议解析
    // 载波频率：38kHz
    // 引导码：4500μs高 + 4500μs低
    // 数据位：560μs低 + 载波（0: 560μs, 1: 1680μs）
    // 数据格式：32位（地址16位 + 命令16位）
    
    if (count < 68) return false;  // Samsung协议至少需要68个时序点（引导码2个+32位*2=66个）
    
    int idx = 0;
    
    // 检查引导码：第一个应该是高电平，持续时间约4500μs（容差±20%）
    if (!timing[idx].level || timing[idx].duration < 3600 || timing[idx].duration > 5400) {
        return false;
    }
    idx++;
    
    if (idx >= count) return false;
    
    // 引导码的低电平部分，约4500μs（容差±20%）
    if (timing[idx].level || timing[idx].duration < 3600 || timing[idx].duration > 5400) {
        return false;
    }
    idx++;
    
    // 解析32位数据
    uint32_t data = 0;
    for (int bit = 0; bit < 32; bit++) {
        if (idx >= count - 1) {
            return false;  // 数据不足
        }
        
        // 每个位由560μs低电平 + 载波时间组成
        // 逻辑0: 560μs载波（容差±20%）
        // 逻辑1: 1680μs载波（容差±20%）
        if (timing[idx].level) {
            return false;  // 应该是低电平
        }
        
        // 放宽容差范围到±20%
        if (timing[idx].duration < 448 || timing[idx].duration > 672) {
            return false;  // 低电平持续时间应该在560μs左右
        }
        idx++;
        
        if (idx >= count) {
            return false;
        }
        
        // 检查载波时间判断是0还是1
        if (!timing[idx].level) {
            return false;  // 应该是高电平（载波）
        }
        
        unsigned long carrierTime = timing[idx].duration;
        // 放宽容差范围到±20%
        if (carrierTime > 1344) {  // 1680 * 0.8 = 1344
            // 逻辑1：载波时间约1680μs
            // ⚠️ TODO（未实机验证）：本行与 parseNEC 同构，推断同样差一个 32 位逐位反序
            //   （库 sendSamsung 也按 MSBfirst 发）。尚未做闭环实测，暂不改行为；
            //   要用 Samsung 设备时先按 ct30w-ir-loopback-verify 的闭环法实测再定。
            data |= (1UL << bit);
        } else if (carrierTime > 448) {  // 560 * 0.8 = 448
            // 逻辑0：载波时间约560μs
            // data位保持0
        } else {
            return false;  // 载波时间异常
        }
        idx++;
    }
    
    // Samsung协议使用32位数据：地址(16位) + 命令(16位)
    signal.protocol = 5;  // Samsung
    signal.data = data;  // 使用完整的32位数据
    signal.useTiming = false;  // 协议信号
    return true;
}

bool IRParser::isNoise(unsigned long duration) const {
    return duration < 50;  // 小于50μs认为是噪声
}

