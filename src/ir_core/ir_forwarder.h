#ifndef IR_FORWARDER_H
#define IR_FORWARDER_H

#include <Arduino.h>
#include "ir_parser.h"
#include "ir_mapper.h"
#include "ir_generator.h"
#include "hardware/ir_transmitter.h"
#include "utils/message_queue.h"
#include "config/constants.h"
#include "config/pins.h"

// 转发器
class IRForwarder {
public:
    IRForwarder(IRMapper& mapper, IRTransmitter& transmitter, MessageQueue& queue);
    bool init();
    void update();
    bool forwardSignal(const SignalID& aSignal);
    bool isForwarding() const;
    void setEnabled(bool enabled);  // 启用/禁用转发功能
    bool isEnabled() const;          // 获取转发功能状态
    bool sendSignal(const SignalID& signal, uint8_t repeatCount = 1);  // 直接发送信号

private:
    IRMapper& mapper;
    IRTransmitter& transmitter;
    MessageQueue& queue;
    bool forwarding;
    bool enabled;  // 转发功能是否启用
    IRGenerator generator;  // 时序生成器
    
    // 预编译的时序数组缓存
    struct CompiledSignal {
        IRTiming* timing;
        int count;
        bool valid;
    };
    
    CompiledSignal compiledSignals[MAX_MAP_RULES][MAX_B_SIGNALS_PER_RULE];
    
    bool compileSignal(const BSignal& signal, IRTiming* timing, int& count);
    bool executeForward(const IR_MapRule& rule);
    void delayPrecise(unsigned long ms);
};

#endif // IR_FORWARDER_H

