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

// 转发器
class IRForwarder {
public:
    IRForwarder(IRMapper& mapper, IRTransmitter& transmitter, MessageQueue& queue, Logger& logger);
    bool init();
    void update();
    bool forwardSignal(const UnifiedSignal& aSignal);
    bool isForwarding() const;
    void setEnabled(bool enabled);  // 启用/禁用转发功能
    bool isEnabled() const;          // 获取转发功能状态
    bool sendSequence(const SignalSequence& sequence);  // 发送信号序列（统一接口，使用序列中信号的repeatCount）
    bool sendSignal(const UnifiedSignal& signal, uint8_t repeatCount = 1);  // 直接发送信号（兼容层，内部调用sendSequence）
    bool sendRawTiming(const IRTiming* timing, int count, uint8_t repeatCount = 1);  // 发送原始时序数据
    bool parseAndSendSignal(const String& payload);  // 统一解析和发送信号（供API使用）

private:
    IRMapper& mapper;
    IRTransmitter& transmitter;
    MessageQueue& queue;
    Logger& logger;  // 日志管理器引用（用于重置信号间隔）
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
    
    // 时序缓冲区（改为成员变量，避免栈溢出）
    IRTiming timingBuffer[300];  // 用于executeForward和sendSignal
    IRTiming learnTimingBuffer[MAX_LEARN_TIMING_COUNT];  // 用于parseAndSendSignal
    
    // 转发延时相关
    PreciseTimer delayTimer;              // 非阻塞延时定时器
    ForwardDelayState delayState;          // 延时状态
    ForwardTask pendingTask;              // 待转发的任务（延时完成后执行）
    bool pendingTaskValid;                // 待转发任务是否有效
    
    bool compileSignal(const UnifiedSignal& signal, IRTiming* timing, int& count);
    bool executeForward(const IR_MapRule& rule);
    void delayPrecise(unsigned long ms);
    
    // 转发相关方法
    void processDelayState();  // 处理延时状态机
    bool createForwardTask(const IR_MapRule& rule, ForwardTask& task);  // 创建转发任务
};

#endif // IR_FORWARDER_H

