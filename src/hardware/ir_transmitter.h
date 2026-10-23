#ifndef IR_TRANSMITTER_H
#define IR_TRANSMITTER_H

#include <Arduino.h>
#include "config/pins.h"
#include "config/constants.h"
#include "hardware/ir_receiver.h"
#include <IRremoteESP8266.h>
#include <IRsend.h>

// 红外发射器
class IRTransmitter {
public:
    IRTransmitter();
    ~IRTransmitter();
    bool init();
    bool transmit(const IRTiming* timing, int count);
    bool transmitRaw(const uint16_t* rawData, int count);
    bool isTransmitting() const;
    
    // 使用IRremoteESP8266库发送（推荐方法）
    bool sendNEC(uint32_t data, uint16_t nbits = 32);
    // 与 sendNEC() **波形逐位相同**，唯一区别是不发尾部那段"补足 NEC 规范命令长度"
    // 的静默（约 44ms）。那段静默不发光，砍掉它不改变被控设备收到的红外，
    // 只让 loop() 提前 44ms 空出来 ⇒ 聋期少 44ms。专供"一次只发一条命令"的转发用。
    // 多信号序列不要用它（会改掉信号之间的间隔）。
    bool sendNECShort(uint32_t data, uint16_t nbits = 32);
    bool sendNECRepeat();  // 发送NEC重复码

    // 距离"上一次发射结束"过去了多少毫秒 —— 收发互斥的静默期判据用。
    // ⚠️ 从未发射过时返回的是 millis()（因为 lastTxEndMs 初始为 0），
    //   在上电头几秒内是个**小值**，不能直接拿来和窗口比。
    //   要判断"是否在静默期"必须先过 hasEverTransmitted() 这道守卫。
    unsigned long msSinceTxEnd() const { return millis() - lastTxEndMs; }

    // 是否至少完整发射过一次
    // ⚠️⚠️ 判据是 lastTxEndMs 而不是 lastTxCode（v1.3.24 评审修正）：
    //   这道守卫的用途是"lastTxEndMs 有没有意义"，而三处写入里只有两处会同时写
    //   lastTxCode —— sendNEC() / sendNECShort() 写，sendNECRepeat() / transmit() /
    //   transmitRaw() **只写 lastTxEndMs**（ir_transmitter.cpp:100/146/183）。
    //   而**转发时序码走的正是 transmit()** ⇒ 用 lastTxCode 判会让"只发过时序码"的
    //   设备恒为 false，把 isBusyTransmitting()（ir_forwarder.cpp:662）与时序码回声
    //   过滤（ir_forwarder.cpp:277）一起废掉 ⇒ 自己发的时序码回波被记成"按键信号"。
    // ⚠️ 这道守卫对 msSinceTxEnd() 是必需的：上电后 millis() 很小，
    //   而 lastTxEndMs=0 ⇒ "距上次发射"看起来像是"刚刚发完"。
    //   （isEchoOfLastTx() 内部是独立的 lastTxCode 守卫：它比的是**码值**，
    //     时序码没有 data 本来就该返回 false，两者用途不同，别合并。）
    bool hasEverTransmitted() const { return lastTxEndMs != 0; }

    // 自收回声判据：code 是否就是"刚发出去的那条命令"，且在 windowMs 之内。
    // 用来替代"按时间屏蔽接收"去挡自家回波 —— 与遥控器协议无关，也不会误伤
    // 真实按键（真实按键的 A 码与被转发的 B 码不是同一个值）。
    bool isEchoOfLastTx(uint32_t code, uint16_t windowMs) const {
        if (code == 0 || lastTxCode == 0) return false;
        return code == lastTxCode && (millis() - lastTxCodeMs) < windowMs;
    }

private:
    IRsend* irsend;  // IRremoteESP8266库的发送器对象
    bool transmitting;
    unsigned long transmitStartTime;
    unsigned long lastTxEndMs;   // 上一次发射结束的时刻（millis()）
    uint32_t lastTxCode;         // 最近一次发出的完整 NEC 码（发重复码时不更新）
    unsigned long lastTxCodeMs;  // 它发出的时刻（millis()）
    
    // ⚠️ 载波**不再自己产生**（v1.3.26 删掉了 enableCarrier/disableCarrier）。
    //   旧实现是 analogWriteFreq(38000) + analogWrite(pin, 512)，注释写"50% 占空比"
    //   是照 0~1023 量程写的 —— 而 ESP8266 core 的默认量程是 **255**
    //   （cores/esp8266/core_esp8266_wiring_pwm.cpp:29 `analogScale = 255`），
    //   512 被钳到 255 ⇒ 占空比 100% ⇒ 发射管恒亮直流、无 38kHz 载波 ⇒ 收方拒收。
    //   现在统一用库自己的 mark()/space()（软件调制），NEC 走的就是它。
};

#endif // IR_TRANSMITTER_H

