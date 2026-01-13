#include "ir_forwarder.h"
#include "utils/precise_timer.h"
#include "ir_core/ir_learner.h"
#include "utils/hex_utils.h"
#include "utils/memory_monitor.h"
#include "unified_signal.h"
#include <ArduinoJson.h>

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
            // 直接从队列数据构建规则，避免重新查找可能失败的问题
            // 队列中已经包含了所有需要的信息（B信号数据、重复次数等）
            IR_MapRule rule;
            
            // 解析A信号（虽然不需要，但保留用于调试）
            rule.aSignal.clear();
            rule.aSignal.protocol = task.signal_data[0] & 0xFF;
            rule.aSignal.data = task.signal_data[1];
            rule.aSignal.isProtocol = true;
            rule.aSignal.useTiming = false;
            rule.aSignal.repeatCount = 1;
            
            // 构建B信号序列
            rule.bSignalCount = task.signal_count;
            for (int i = 0; i < rule.bSignalCount && i < MAX_B_SIGNALS_PER_RULE; i++) {
                rule.bSignals[i].clear();
                rule.bSignals[i].protocol = task.b_protocols[i];  // 使用队列中存储的协议信息
                rule.bSignals[i].data = task.signal_data[i + 2];  // B信号数据从signal_data[2]开始
                rule.bSignals[i].isProtocol = true;  // 假设都是协议信号（队列中存储的是data）
                rule.bSignals[i].useTiming = false;
                rule.bSignals[i].repeatCount = 1;
            }
            
            // 设置间隔和重复次数
            for (int i = 0; i < rule.bSignalCount - 1 && i < MAX_B_SIGNALS_PER_RULE - 1; i++) {
                rule.intervals[i] = task.intervals[i];
            }
            rule.repeatCount = task.repeat_count;
            rule.valid = true;
            
            // 直接执行转发，使用队列中的数据
            executeForward(rule);
        }
    }
}

bool IRForwarder::forwardSignal(const UnifiedSignal& aSignal) {
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
    
    // B信号数据从signal_data[2]开始存储，同时存储协议信息
    for (int i = 0; i < rule->bSignalCount && i < 3; i++) {  // 最多3个B信号（signal_data[2-4]）
        if (rule->bSignals[i].useTiming) {
            // 使用时序码：存储时序码Base64字符串的哈希（简化处理）
            task.signal_data[i + 2] = rule->bSignals[i].timingBase64.length();  // 临时存储长度
        } else {
            // 使用协议：存储data
            task.signal_data[i + 2] = rule->bSignals[i].data;
        }
        task.b_protocols[i] = rule->bSignals[i].protocol;  // 存储B信号协议
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

bool IRForwarder::sendSignal(const UnifiedSignal& signal, uint8_t repeatCount) {
    if (repeatCount == 0) repeatCount = 1;
    
    // 如果使用时序码，直接发送时序码
    if (signal.useTiming) {
        // 创建非const副本用于加载时序码
        UnifiedSignal signalCopy = signal;
        if (!signalCopy.loadTiming()) {
            return false;
        }
        
        for (uint8_t i = 0; i < repeatCount; i++) {
            if (!transmitter.transmit(signalCopy.timing, signalCopy.timingCount)) {
                signalCopy.freeTiming();
                return false;
            }
            if (i < repeatCount - 1) {
                delay(100);  // 重复发送之间的延迟
            }
        }
        
        signalCopy.freeTiming();
        return true;
    }
    
    // 使用协议发送
    if (signal.isProtocol && signal.protocol == 1) {
        // NEC协议
        for (uint8_t i = 0; i < repeatCount; i++) {
            if (i == 0) {
                transmitter.sendNEC(signal.data, 32);
            } else {
                delay(10);  // 最小延迟，确保信号完整发送
                transmitter.sendNECRepeat();
            }
            if (i < repeatCount - 1) {
                delay(40);  // 重复码间隔，减少到40ms（从50ms减少）
            }
        }
        return true;
    } else if (signal.isProtocol) {
        // 其他协议：使用时序生成器
        int count = 0;
        if (generator.generateTiming(signal.protocol, signal.data, timingBuffer, count, 300)) {
            for (uint8_t i = 0; i < repeatCount; i++) {
                transmitter.transmit(timingBuffer, count);
                if (i < repeatCount - 1) {
                    delay(100);
                }
            }
            return true;
        }
    }
    
    return false;
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
        return SignalUtils::base64ToTiming(signal.timingBase64, timing, count, 300);
    } else if (signal.isProtocol) {
        // 协议信号：使用时序生成器生成时序
        return generator.generateTiming(signal.protocol, signal.data, timing, count, 300);
    } else {
        count = 0;
        return false;
    }
}

bool IRForwarder::executeForward(const IR_MapRule& rule) {
    forwarding = true;
    unsigned long startTime = millis();
    
    // 检查第一个信号是否使用时序码
    bool useTiming = (rule.bSignalCount > 0 && rule.bSignals[0].useTiming);
    
    // 检查第一个信号是否为NEC协议（且不使用时序码）
    bool isNEC = (rule.bSignalCount > 0 && 
                  !rule.bSignals[0].useTiming &&
                  rule.bSignals[0].isProtocol && 
                  rule.bSignals[0].protocol == 1);
    
    if (useTiming) {
        // 使用时序码：直接发送时序码，包含精确的延迟
        for (uint8_t repeat = 0; repeat < rule.repeatCount; repeat++) {
            // 发射每个B信号
            for (uint8_t i = 0; i < rule.bSignalCount; i++) {
                const UnifiedSignal& signal = rule.bSignals[i];
                
                if (signal.useTiming && signal.hasTiming()) {
                    // 加载时序码（创建非const副本）
                    UnifiedSignal signalCopy = signal;
                    if (signalCopy.loadTiming()) {
                        transmitter.transmit(signalCopy.timing, signalCopy.timingCount);
                        signalCopy.freeTiming();
                    }
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
    } else if (isNEC) {
        // NEC协议：发送1次完整信号 + (repeatCount-1)次重复码
        // 注意：开关机应该使用时序码，这里只处理其他按键（如音量、静音等）
        // 这些按键不需要额外的延迟，直接按NEC协议标准发送即可
        const UnifiedSignal& signal = rule.bSignals[0];
        
        // 发送第一个完整信号
        transmitter.sendNEC(signal.data, 32);
        
        // 发送重复码（如果repeatCount > 1）
        if (rule.repeatCount > 1) {
            for (uint8_t r = 1; r < rule.repeatCount; r++) {
                delay(10);  // 最小延迟，确保信号完整发送
                transmitter.sendNECRepeat();
                
                if (r < rule.repeatCount - 1) {
                    delay(40);  // 重复码间隔（标准NEC协议间隔）
                }
            }
        }
    } else {
        // 其他协议：使用自定义时序生成
        
        // 执行重复发射
        for (uint8_t repeat = 0; repeat < rule.repeatCount; repeat++) {
            // 发射每个B信号
            for (uint8_t i = 0; i < rule.bSignalCount; i++) {
                const UnifiedSignal& signal = rule.bSignals[i];
                
                int timingCount = 0;
                if (compileSignal(signal, timingBuffer, timingCount) && timingCount > 0) {
                    transmitter.transmit(timingBuffer, timingCount);
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

bool IRForwarder::parseAndSendSignal(const String& payload) {
    String trimmed = payload;
    trimmed.trim();
    
    if (trimmed.length() == 0) {
        return false;
    }
    
    // 检测格式
    IRLearner::SignalFormat format = IRLearner::detectSignalFormat(trimmed);
    
    if (format == IRLearner::FORMAT_SIGNALID) {
        // UnifiedSignal对象格式（JSON）
        size_t jsonSize = trimmed.length() + 256;
        if (jsonSize > 1024) jsonSize = 1024;
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
        
        JsonObject signalObj = doc["signal"];
        if (signalObj.isNull()) {
            return false;
        }
        
        uint8_t repeatCount = doc["repeatCount"] | 1;
        
        UnifiedSignal signal;
        if (!SignalUtils::signalFromJSON(signalObj, signal)) {
            return false;
        }
        signal.repeatCount = repeatCount;
        
        return sendSignal(signal, repeatCount);
    } else {
        // 字符串格式（Base64/数组/协议），使用统一解析函数
        UnifiedSignal signal;
        int timingCount = 0;
        uint8_t repeatCount = 1;
        
        if (!IRLearner::parseSignalPayload(trimmed, signal, learnTimingBuffer, timingCount, MAX_LEARN_TIMING_COUNT, repeatCount, format)) {
            return false;
        }
        
        // 根据格式发送信号
        if (format == IRLearner::FORMAT_PROTOCOL) {
            signal.repeatCount = repeatCount;
            return sendSignal(signal, repeatCount);
        } else {
            return sendRawTiming(learnTimingBuffer, timingCount, repeatCount);
        }
    }
}
