#include "ir_forwarder.h"
#include "utils/precise_timer.h"
#include "ir_core/ir_learner.h"
#include "utils/hex_utils.h"
#include "utils/memory_monitor.h"
#include "utils/logger.h"
#include "unified_signal.h"
#include <ArduinoJson.h>

IRForwarder::IRForwarder(IRMapper& mapper, IRTransmitter& transmitter, MessageQueue& queue, Logger& logger)
    : mapper(mapper), transmitter(transmitter), queue(queue), logger(logger), forwarding(false), enabled(true),
      delayState(FORWARD_DELAY_IDLE), pendingTaskValid(false) {
    // 初始化预编译缓存
    for (int i = 0; i < MAX_MAP_RULES; i++) {
        for (int j = 0; j < MAX_B_SIGNALS_PER_RULE; j++) {
            compiledSignals[i][j].timing = nullptr;
            compiledSignals[i][j].count = 0;
            compiledSignals[i][j].valid = false;
        }
    }
    
    // 简化转发逻辑，不再需要重复码相关状态
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
    // 处理延时状态机
    processDelayState();
    
    // 如果延时完成且有待转发任务，执行转发
    if (delayState == FORWARD_DELAY_READY && pendingTaskValid) {
        const ForwardTask& t = pendingTask;
        if (t.signal_count == 1 && t.b_protocols[0] == 1) {
            forwarding = true;
            if (t.intervals[0] > 0) delayPrecise(t.intervals[0]);
            transmitter.sendNEC(t.signal_data[2], 32);
            forwarding = false;
            logger.resetSignalInterval();
        } else {
            IR_MapRule rule;
            if (buildRuleFromTask(pendingTask, rule)) {
                executeForward(rule);
            }
        }
        pendingTaskValid = false;
        delayState = FORWARD_DELAY_IDLE;
    }
    
    // 优化：如果正在转发，快速返回，避免阻塞
    if (forwarding) {
        return;
    }
    
    // 从消息队列获取转发任务
    ForwardTask task;
    if (queue.pop(task) && task.valid) {
        if (task.signal_count == 1 && task.b_protocols[0] == 1) {
            forwarding = true;
            if (task.intervals[0] > 0) delayPrecise(task.intervals[0]);
            transmitter.sendNEC(task.signal_data[2], 32);
            forwarding = false;
            logger.resetSignalInterval();
        } else {
            IR_MapRule rule;
            if (buildRuleFromTask(task, rule)) {
                executeForward(rule);
            }
        }
    }
}

bool IRForwarder::forwardSignal(const UnifiedSignal& aSignal) {
    if (!enabled) {
        return false;  // 转发功能已禁用
    }
    
    // 简化逻辑：直接查找规则并转发，不再区分完整信号和重复码
    IR_MapRule* rule = mapper.findRule(aSignal);
    if (rule == nullptr) {
        return false;  // 未找到规则，不转发
    }
    
    // 含时序码 B 信号的规则无法经 queue/pendingTask 还原，仅同步执行
    for (int i = 0; i < rule->bSequence.signalCount; i++) {
        if (rule->bSequence.signals[i].useTiming) {
            executeForward(*rule);
            return true;
        }
    }
    
    // 创建转发任务
    ForwardTask task;
    if (!createForwardTask(*rule, task)) {
        return false;
    }
    
    // 开始非阻塞延时
    if (delayTimer.startDelay(FORWARD_DELAY_BEFORE_SEND_MS)) {
        delayState = FORWARD_DELAY_WAITING;
        pendingTask = task;
        pendingTaskValid = true;
        return true;
    } else {
        // 延时启动失败，直接通过队列转发
        if (!queue.push(task)) {
            logger.log("warning", "转发队列已满，丢弃任务");
            return false;
        }
        return true;
    }
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
    
    // 使用序列中第一个信号的repeatCount（如果>1，重复发送整个序列）
    uint8_t repeatCount = 1;
    if (sequence.signalCount > 0 && sequence.signals[0].repeatCount > 1) {
        repeatCount = sequence.signals[0].repeatCount;
        if (repeatCount > SIGNAL_REPEAT_MAX) {
            repeatCount = SIGNAL_REPEAT_MAX;
        }
    }
    
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
        for (uint8_t repeat = 0; repeat < repeatCount; repeat++) {
            // 如果有第一个信号前的间隔，先等待
            if (hasFirstInterval && sequence.intervals[0] > 0) {
                delayPrecise(sequence.intervals[0]);
            }
            
            // 发射每个信号
            for (uint8_t i = 0; i < sequence.signalCount; i++) {
                const UnifiedSignal& signal = sequence.signals[i];
                
                if (signal.useTiming && signal.hasTiming()) {
                    // 加载时序码（创建非const副本）
                    UnifiedSignal signalCopy = signal;
                    if (signalCopy.loadTiming()) {
                        transmitter.transmit(signalCopy.timing, signalCopy.timingCount);
                        signalCopy.freeTiming();
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
            if (repeat < repeatCount - 1) {
                delayPrecise(100);  // 默认100ms重复间隔
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
            
            transmitter.sendNEC(signal.data, 32);
            
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
    return true;
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
        return SignalUtils::base64ToTiming(signal.timingBase64, timing, count, 300);
    } else if (signal.isProtocol()) {
        // 协议信号：使用时序生成器生成时序
        return generator.generateTiming(signal.protocol, signal.data, timing, count, 300);
    } else {
        count = 0;
        return false;
    }
}

bool IRForwarder::executeForward(const IR_MapRule& rule) {
    forwarding = true;
    
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
        if (!IRLearner::parseStringSequence(trimmed, sequence, learnTimingBuffer, MAX_LEARN_TIMING_COUNT)) {
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

// ForwardTask 最多支持 3 个 B 信号（与 message_queue.h 中 b_protocols[3]、signal_data[2..4] 一致）
static const int FORWARD_TASK_MAX_B = 3;

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
    bool hasFirstInterval = (task.signal_count > 1 && task.intervals[task.signal_count - 1] > 0) ||
                            (task.signal_count == 1 && task.intervals[0] > 0);
    int n = (task.signal_count < FORWARD_TASK_MAX_B) ? task.signal_count : FORWARD_TASK_MAX_B;
    for (int i = 0; i < n; i++) {
        UnifiedSignal bSignal;
        bSignal.clear();
        bSignal.protocol = task.b_protocols[i];
        bSignal.data = task.signal_data[i + 2];
        bSignal.useTiming = false;
        bSignal.repeatCount = 1;
        uint16_t interval = 0;
        bool isFirstInterval = false;
        if (hasFirstInterval) {
            isFirstInterval = true;
            if (i == 0) {
                interval = task.intervals[0];
            } else if (i < 4) {
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
    task.signal_id = 0;  // 不再使用signal_id
    task.signal_data[0] = rule.aSignal.protocol;  // protocol
    task.signal_data[1] = rule.aSignal.data;  // 完整的32位data
    // ForwardTask 最多支持 3 个 B 信号，与 signal_data[2..4]、b_protocols[3] 一致，避免 update() 越界
    int signalCount = (rule.bSequence.signalCount <= 3) ? rule.bSequence.signalCount : 3;
    task.signal_count = static_cast<uint8_t>(signalCount);
    task.rule_id = rule.id;  // 保存规则ID用于调试
    
    // B信号数据从signal_data[2]开始存储，同时存储协议信息
    for (int i = 0; i < signalCount; i++) {  // 最多3个B信号（signal_data[2-4]）
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
        for (int i = 0; i < intervalCount && i < 4; i++) {
            task.intervals[i] = rule.bSequence.intervals[i];
        }
    } else {
        // 标准模式：只存储信号之间的间隔
        for (int i = 0; i < signalCount - 1 && i < 2; i++) {
            task.intervals[i] = rule.bSequence.intervals[i];
        }
    }
    
    // 注意：序列不再包含repeatCount，使用默认值1
    task.repeat_count = 1;
    task.valid = true;
    
    return true;
}
