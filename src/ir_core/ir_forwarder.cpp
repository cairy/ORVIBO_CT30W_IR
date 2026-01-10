#include "ir_forwarder.h"
#include "utils/precise_timer.h"

IRForwarder::IRForwarder(IRMapper& mapper, IRTransmitter& transmitter, MessageQueue& queue)
    : mapper(mapper), transmitter(transmitter), queue(queue), forwarding(false), enabled(true) {
    // 初始化预编译缓存
    for (int i = 0; i < MAX_MAP_RULES; i++) {
        for (int j = 0; j < MAX_B_SIGNALS_PER_RULE; j++) {
            compiledSignals[i][j].timing = nullptr;
            compiledSignals[i][j].count = 0;
            compiledSignals[i][j].valid = false;
        }
    }
}

bool IRForwarder::init() {
    // 初始化时序生成器
    if (!generator.init()) {
        return false;
    }
    
    // 预编译缓存初始化（现在统一使用时序生成，不需要预编译）
    // 保留缓存结构以备将来优化使用
    for (int i = 0; i < MAX_MAP_RULES; i++) {
        for (int j = 0; j < MAX_B_SIGNALS_PER_RULE; j++) {
            compiledSignals[i][j].timing = nullptr;
            compiledSignals[i][j].count = 0;
            compiledSignals[i][j].valid = false;
        }
    }
    
    return true;
}

void IRForwarder::update() {
    // 优化：如果正在转发，快速返回，避免阻塞
    if (forwarding) {
        return;
    }
    
    // 从消息队列获取转发任务
    ForwardTask task;
    if (queue.pop(task)) {
        if (task.valid) {
            // 解析信号ID：使用signal_data[0]存储protocol，signal_data[1]存储data
            // 这样可以避免24位截断问题
            SignalID aSignal;
            aSignal.protocol = task.signal_data[0] & 0xFF;  // protocol在低8位
            aSignal.data = task.signal_data[1];  // data完整32位
            aSignal.isProtocol = true;
            
            // 查找映射规则并执行转发
            IR_MapRule* rule = mapper.findRule(aSignal);
            if (rule != nullptr) {
                executeForward(*rule);
            }
        }
    }
}

bool IRForwarder::forwardSignal(const SignalID& aSignal) {
    if (!enabled) {
        return false;  // 转发功能已禁用
    }
    
    IR_MapRule* rule = mapper.findRule(aSignal);
    if (rule == nullptr) {
        return false;
    }
    
    // 通过消息队列提交任务
    ForwardTask task;
    // 使用signal_data[0]存储protocol，signal_data[1]存储data，避免24位截断
    task.signal_id = 0;  // 不再使用signal_id
    task.signal_data[0] = aSignal.protocol;  // protocol
    task.signal_data[1] = aSignal.data;  // 完整的32位data
    task.signal_count = rule->bSignalCount;
    
    // B信号数据从signal_data[2]开始存储
    for (int i = 0; i < rule->bSignalCount && i < 3; i++) {  // 最多3个B信号（signal_data[2-4]）
        task.signal_data[i + 2] = (rule->bSignals[i].isProtocol) ?
            rule->bSignals[i].data :  // 直接存储data，避免截断
            rule->bSignals[i].hash;
    }
    
    for (int i = 0; i < rule->bSignalCount - 1; i++) {
        task.intervals[i] = rule->intervals[i];
    }
    
    task.repeat_count = rule->repeatCount;
    task.valid = true;
    
    return queue.push(task);
}

void IRForwarder::setEnabled(bool enabled) {
    this->enabled = enabled;
}

bool IRForwarder::isEnabled() const {
    return enabled;
}

bool IRForwarder::sendSignal(const SignalID& signal, uint8_t repeatCount) {
    if (repeatCount == 0) repeatCount = 1;
    
    // 直接发送信号，不经过转发规则
    if (signal.isProtocol && signal.protocol == 1) {
        // NEC协议
        for (uint8_t i = 0; i < repeatCount; i++) {
            if (i == 0) {
                transmitter.sendNEC(signal.data, 32);
            } else {
                delay(20);
                transmitter.sendNECRepeat();
            }
            if (i < repeatCount - 1) {
                delay(50);
            }
        }
        return true;
    } else {
        // 其他协议：使用时序生成器
        static IRTiming timing[300];
        int count = 0;
        
        if (signal.isProtocol) {
            if (generator.generateTiming(signal.protocol, signal.data, timing, count, 300)) {
                for (uint8_t i = 0; i < repeatCount; i++) {
                    transmitter.transmit(timing, count);
                    if (i < repeatCount - 1) {
                        delay(100);
                    }
                }
                return true;
            }
        }
        // 哈希信号需要从学习模式中恢复时序，这里暂时不支持
        return false;
    }
}

bool IRForwarder::isForwarding() const {
    return forwarding || transmitter.isTransmitting();
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
    
    return true;
}

bool IRForwarder::compileSignal(const BSignal& signal, IRTiming* timing, int& count) {
    // 统一使用时序生成器编译信号
    // 对于协议信号，使用IRGenerator生成时序
    // 对于哈希信号，需要从学习模式保存的时序中恢复
    
    if (timing == nullptr) {
        count = 0;
        return false;
    }
    
    if (signal.isProtocol) {
        // 协议信号：使用时序生成器生成时序
        return generator.generateTiming(signal.protocol, signal.data, timing, count, 300);
    } else {
        // 哈希信号需要从存储中恢复时序
        // TODO: 实现从存储中加载时序的逻辑
        // 目前返回false，表示无法编译
        count = 0;
        return false;
    }
}

bool IRForwarder::executeForward(const IR_MapRule& rule) {
    forwarding = true;
    unsigned long startTime = millis();
    
    // 检查第一个信号是否为NEC协议
    bool isNEC = (rule.bSignalCount > 0 && 
                  rule.bSignals[0].isProtocol && 
                  rule.bSignals[0].protocol == 1);
    
    if (isNEC) {
        // NEC协议：发送1次完整信号 + (repeatCount-1)次重复码
        const BSignal& signal = rule.bSignals[0];
        
        // 优化：减少延迟时间，在保证时序的前提下提升响应速度
        if (rule.repeatCount > 1) {
            // 电源键等需要重复的信号：减少延迟到30ms
            delay(30);
        } else {
            // 音量、静音等单次信号：减少延迟到20ms
            delay(20);
        }
        
        transmitter.sendNEC(signal.data, 32);
        
        // 发送重复码（如果repeatCount > 1）
        if (rule.repeatCount > 1) {
            delay(20);  // 减少延迟到20ms
            for (uint8_t r = 1; r < rule.repeatCount; r++) {
                transmitter.sendNECRepeat();
                if (r < rule.repeatCount - 1) {
                    delay(50);  // 减少延迟到50ms
                }
            }
        }
    } else {
        // 其他协议：使用自定义时序生成
        static IRTiming timing[300];
        
        // 执行重复发射
        for (uint8_t repeat = 0; repeat < rule.repeatCount; repeat++) {
            // 发射每个B信号
            for (uint8_t i = 0; i < rule.bSignalCount; i++) {
                const BSignal& signal = rule.bSignals[i];
                
                int timingCount = 0;
                if (compileSignal(signal, timing, timingCount) && timingCount > 0) {
                    transmitter.transmit(timing, timingCount);
                }
                
                // 信号间隔（最后一个信号后不需要间隔）
                if (i < rule.bSignalCount - 1) {
                    delayPrecise(rule.intervals[i]);
                }
            }
            
            // 重复间隔（最后一次重复后不需要间隔）
            if (repeat < rule.repeatCount - 1) {
                delayPrecise(100);  // 默认100ms重复间隔
            }
        }
    }
    
    // 优化：延迟检查移到循环外，减少不必要的检查
    // 只在最后检查一次总延迟
    unsigned long elapsed = millis() - startTime;
    if (elapsed > FORWARD_DELAY_MAX_MS) {
        // 延迟超限，记录警告（可通过Logger记录）
    }
    
    forwarding = false;
    return true;
}

void IRForwarder::delayPrecise(unsigned long ms) {
    // 使用硬件定时器实现精准延时（误差≤20ms）
    PreciseTimer::delayPreciseMs(ms);
}

