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
    bool sendNECRepeat();  // 发送NEC重复码

private:
    IRsend* irsend;  // IRremoteESP8266库的发送器对象
    bool transmitting;
    unsigned long transmitStartTime;
    
    void enableCarrier(uint32_t freqHz = 38000);
    void disableCarrier();
    void delayMicrosecondsPrecise(unsigned long us);
};

#endif // IR_TRANSMITTER_H

