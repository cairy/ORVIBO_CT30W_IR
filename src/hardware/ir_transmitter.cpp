#include "ir_transmitter.h"
#include "utils/precise_timer.h"
#include <ir_NEC.h>   // kNecHdrMark / kNecHdrSpace / kNecBitMark / kNecOneSpace / kNecZeroSpace

IRTransmitter::IRTransmitter() 
    : irsend(nullptr), transmitting(false), transmitStartTime(0), lastTxEndMs(0),
      lastTxCode(0), lastTxCodeMs(0) {
}

IRTransmitter::~IRTransmitter() {
    if (irsend != nullptr) {
        delete irsend;
        irsend = nullptr;
    }
}

bool IRTransmitter::init() {
    // 初始化IRremoteESP8266库的发送器
    if (irsend == nullptr) {
        irsend = new IRsend(IR_TX_PIN);
        if (irsend == nullptr) {
            return false;
        }
    }
    
    irsend->begin();
    
    // 保留原有的初始化逻辑
    pinMode(IR_TX_PIN, OUTPUT);
    digitalWrite(IR_TX_PIN, LOW);
    return true;
}

bool IRTransmitter::sendNEC(uint32_t data, uint16_t nbits) {
    if (irsend == nullptr) {
        return false;
    }
    
    transmitting = true;
    irsend->sendNEC(data, nbits);
    transmitting = false;
    lastTxEndMs = millis();
    lastTxCode = data;
    lastTxCodeMs = lastTxEndMs;
    return true;
}

// 与 IRsend::sendNEC() **逐参数相同**，唯一区别：不"补足 NEC 规范命令长度"。
//
// 库里的路径：IRsend::sendNEC → sendGeneric(..., gap = kNecMinGap,
//   mesgtime = kNecMinCommandLength = 193×560µs = 108.08ms)，收尾执行
//     if (elapsed >= mesgtime) space(gap); else space(max(gap, mesgtime - elapsed));
// 以 0x011B08F7（音量+）实算：真实波形（引导码 13.44ms + 32 位 53.4ms + 收尾 mark 0.56ms）
// = 64.4ms，收尾补到 108.08ms ⇒ 那 43.7ms 全是 `space()` —— **完全不发光**。
// 这段静默是给"NEC 连续帧之间留间隔"用的，对"一次只发一条命令"毫无作用，
// 但 loop() 要白等 43.7ms，而聋期是按发射占用算的，白等一样算聋。
//（实测 durationMs=108 就是 64.4 + 43.7。）
//
// 所以这里把 mesgtime 与 gap 都传 0：sendGeneric 会走 `space(0)`，
// 而 `IRsend::space()` 的实现是 `ledOff(); if (time == 0) return;`（IRsend.cpp:194）
// ⇒ 灯照样关掉，只是不等待。**灯亮的时间一秒不差**，被控设备收到的红外一字不差。
//
// ⚠️ 只用于"一次只发一条命令"。多信号序列（如规则 134 电源 = 整帧 + 2×重复码）
//    砍掉收尾会让信号之间的实际间隔短 43.7ms、跟学到的间隔对不上 ⇒ 那里仍用 sendNEC()。
bool IRTransmitter::sendNECShort(uint32_t data, uint16_t nbits) {
    if (irsend == nullptr) {
        return false;
    }
    if (nbits == 0 || nbits > 32) nbits = 32;

    transmitting = true;
    irsend->sendGeneric(kNecHdrMark, kNecHdrSpace,
                        kNecBitMark, kNecOneSpace,
                        kNecBitMark, kNecZeroSpace,
                        kNecBitMark,
                        (uint32_t)0,      // gap：不等待
                        (uint32_t)0,      // mesgtime：不补足命令总长
                        (uint64_t)data, nbits,
                        38,               // 38kHz
                        true,             // MSB first（与 sendNEC 一致）
                        0,                // 不重复
                        33);              // 占空比 33%（与 sendNEC 一致）
    transmitting = false;
    lastTxEndMs = millis();
    lastTxCode = data;
    lastTxCodeMs = lastTxEndMs;
    return true;
}

bool IRTransmitter::sendNECRepeat() {
    if (irsend == nullptr) {
        return false;
    }
    
    transmitting = true;
    // NEC重复码波形：9000μs高 + 2250μs低 + 560μs低
    uint16_t necRepeat[3] = {9000, 2250, 560};
    irsend->sendRaw(necRepeat, 3, 38);  // 38kHz载波
    transmitting = false;
    lastTxEndMs = millis();
    return true;
}

bool IRTransmitter::transmit(const IRTiming* timing, int count) {
    if (timing == nullptr || count <= 0) {
        return false;
    }
    
    transmitting = true;
    transmitStartTime = micros();
    const unsigned long _tMs0 = millis();
    unsigned int _yields = 0;      // 发射期间让出过几次（埋点用 v1.3.30；v1.3.57 起不再关中断）
    
    // ⚠️⚠️ 载波必须用**库自己的** mark()/space()（v1.3.26 修正）。
    //   旧写法是自制的 enableCarrier()：analogWriteFreq(38000) + analogWrite(pin, 512)，
    //   注释写"50% 占空比"是照 0~1023 量程写的 —— 而 ESP8266 core 的默认量程是 **255**
    //   （cores/esp8266/core_esp8266_wiring_pwm.cpp:29 `analogScale = 255`），
    //   512 被钳到 255 ⇒ 占空比 100% ⇒ 发射管**恒亮直流**，一个 38kHz 载波都没有。
    //   38kHz 接收头（TSOP 系列）对直流光是拒收的 ⇒ NEC 能转发、时序码永远发不出去。
    //   顺带解释"发完设备就掉线"：恒亮时 LED 平均电流约是 50% 调制的 2 倍，
    //   3.3V 轨被拉低时**射频段先饿死**（CPU 还活着、WiFi 全断、串口不出字）。
    //   （时序码发射路径自上线起从未被真正验证 —— TASKS.md 第 14 节量的是耗时/durationMs，
    //     不是"被控设备是否收到"。）
    //
    //   库的 mark() 是软件调制：ledOn→delay(onTimePeriod)→ledOff→delay(offTimePeriod) 循环，
    //   被控设备真正收到的那条 NEC 走的就是它 ⇒ 本工程唯一被实机证明能用的载波方式。
    //
    //   ⭐ v1.3.56：占空比 50% → 33%（与 NEC 路径 `sendGeneric(..., 33)` 一致）。
    //   ⇒ **实测无效**（111）：33% 下仍 **6.4 min 僵死**（50% 时 10.5 / 12 min，3 轮合计）
    //     ⇒ "占空比把 3.3V 轨拉低"这条假说被排除。33% 保留（更省电，NEC 规范本就是 1/3）。
    //
    //   ⭐⭐ v1.3.57（本次修复）：**去掉发射期间的关中断**。
    //   判据（111，突发连点档，除注明外参数逐字相同）：
    //     · 时序码（**关中断**）6 条 × 190ms = 1.14 s/组 ⇒ **6.4 / 10.5 / 12 min 必僵死**（3 轮）
    //     · NEC（**不关中断**）24 条 × 65ms = 1.54 s/组 —— **总发光比时序码还多 35%** ——
    //       ⇒ **3216 条 / 20 min 零僵死**（J 档；原本以为的元凶"次数/时长/占空比"它样样更大）
    //   ⇒ 元凶是**关中断**本身，与发射能量无关。
    //   机理：每 10ms 才开一次中断，而开窗只有一次 yield() 的时长（几十 µs）⇒
    //   190ms 里 >99% 的时间屏蔽 802.11 MAC ⇒ 丢 beacon / 下行帧、MQTT keepalive 超时。
    //   僵死现场的三条特征全部吻合：freeHeap 健康（9.4~12 KB）、CPU 仍在跑（heap 与
    //   看门狗日志照常打印）、只有网络不可达（ping 网关 151ms 超时 3 次后自愈重启）。
    //   NEC 路径（sendGeneric）本来就不关中断，是本工程唯一被实机证明可用的载波方式。
    irsend->enableIROut(38, 33);   // 38kHz（<1000 视为 kHz）；占空比 33%

    // ⭐ v1.3.57：**不再 noInterrupts()**（理由见上）。
    //   ⚠️ 连带删掉 longPulse 的"临时开中断"分支：不关中断后，库的 _delayMicroseconds
    //   对 >16383µs 的单点改走 delay()（内部 yield）是安全的，不需要特殊照顾。
    //   ⚠️ 保留每 10ms 一次的 yield()：WiFi 自己已能跑，但主循环里的 NetWatchdog / HTTP
    //   仍需要让出，长帧才有机会被服务。
    for (int i = 0; i < count; i++) {
        const uint32_t d = timing[i].duration;

        if (timing[i].level) {
            // mark() 收 uint16_t，超长会回绕 ⇒ 钳住
            irsend->mark((uint16_t)(d > 65535UL ? 65535UL : d));
        } else {
            irsend->space(d);
        }

        // 按**累计发射时长**判定让出（判据是时间不是点数：Sony 12 位 26 点 ≈ 14ms、
        // RC6 45 点 ≈ 25ms，点数与时长没有对应关系；旧写法 `i % 100` 对短帧永不命中）
        if ((micros() - transmitStartTime) >= (INTERRUPT_DISABLE_MAX_MS * 1000UL)) {
            yield();            // 让出CPU时间
            transmitStartTime = micros();  // 重置计时器
            _yields++;                     // 埋点（v1.3.30）
        }
    }
    
    irsend->space(0);   // ledOff()：确保发射管关闭（不发光、不发热）
    
    transmitting = false;
    lastTxEndMs = millis();

    // ── 埋点（v1.3.30）：关中断窗口到底让出去几次，以及整段占了多少时间 ──
    // 只在与"卡死"相关的路径上打一行；让出次数为 0 时不打，免得日志淹掉现场。
    if (_yields > 0 || (millis() - _tMs0) > 50UL) {
        Serial.printf_P(PSTR("[TX] transmit pts=%d yields=%u ms=%lu\n"),
                      count, _yields, millis() - _tMs0);
    }
    return true;
}

bool IRTransmitter::transmitRaw(const uint16_t* rawData, int count) {
    if (rawData == nullptr || count <= 0) {
        return false;
    }
    
    transmitting = true;
    transmitStartTime = micros();
    
    // 载波同 transmit()：必须用库的 mark()/space()，不能自己 analogWrite（见上）
    // v1.3.56：占空比同步降到 33%；v1.3.57：同步**去掉关中断**（理由见 transmit()）
    irsend->enableIROut(38, 33);

    for (int i = 0; i < count; i++) {
        bool level = (i % 2 == 0);  // 交替高低电平
        const uint32_t d = rawData[i];
        if (level) {
            irsend->mark((uint16_t)(d > 65535UL ? 65535UL : d));
        } else {
            irsend->space(d);
        }

        // 同上：按累计发射时长判定让出，不再按点数（i % 100 对短帧永不命中）
        if ((micros() - transmitStartTime) >= (INTERRUPT_DISABLE_MAX_MS * 1000UL)) {
            yield();            // 让出CPU时间
            transmitStartTime = micros();  // 重置计时器
        }
    }
    
    irsend->space(0);   // ledOff()
    
    transmitting = false;
    lastTxEndMs = millis();
    return true;
}

bool IRTransmitter::isTransmitting() const {
    return transmitting;
}

