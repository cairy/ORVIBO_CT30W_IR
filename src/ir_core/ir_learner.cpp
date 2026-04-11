#include "ir_learner.h"
#include "utils/hex_utils.h"
#include "unified_signal.h"
#include <ArduinoJson.h>
#include <string.h>


IRLearner::IRLearner(IRReceiver& receiver, IRParser& parser, Logger& logger)
    : receiver(receiver), parser(parser), logger(logger),
      mode(LEARN_NONE),
      learnedDevice(nullptr),
      currentCommandIndex(0), commandNamesCount(0), lastSignalTime(0), learnTimeoutMs(0),
      hasLastSignal(false), lastLearnedSignalTime(0) {
    lastLearnedSignal.clear();
}

void IRLearner::init() {
    mode = LEARN_NONE;
    freeLearnedDevice();  // 确保没有遗留的内存
}

void IRLearner::update() {
    // 检查设备学习超时
    if (mode == LEARN_DEVICE) {
        checkLearnTimeout();
    }
    
    if (mode == LEARN_DEVICE) {
        // 设备学习模式
        static IRTiming timing[MAX_LEARN_TIMING_COUNT];
        int count = 0;
        
        if (receiver.getSignal(timing, count, MAX_LEARN_TIMING_COUNT)) {
            lastSignalTime = millis();
            processDeviceLearnSignal(timing, count);
        }
    }
}

LearnMode IRLearner::getMode() const {
    return mode;
}

bool IRLearner::startDeviceLearn(const String& deviceName, const String& commandNames, int timeoutSeconds) {
    if (mode != LEARN_NONE) {
        return false;  // 已有学习任务进行中
    }
    
    // 检查内存是否足够分配LearnedDevice
    size_t freeHeap = ESP.getFreeHeap();
    size_t requiredSize = sizeof(LearnedDevice);
    if (freeHeap < requiredSize + 1024) {  // 需要至少1KB余量
        Serial.printf("[IRLearner] 内存不足，无法开始学习 (需要: %d bytes, 可用: %d bytes)\n", 
                     requiredSize, freeHeap);
        return false;
    }
    
    // 释放之前的学习数据（如果存在）
    freeLearnedDevice();
    
    // 分配新的学习设备内存
    learnedDevice = new (std::nothrow) LearnedDevice();
    if (learnedDevice == nullptr) {
        Serial.printf("[IRLearner] 内存分配失败，无法开始学习 (需要: %d bytes)\n", requiredSize);
        return false;
    }
    
    // 初始化学习设备
    learnedDevice->valid = false;
    learnedDevice->commandCount = 0;
    learnedDevice->deviceName[0] = '\0';
    
    // 禁用IRremoteESP8266库的接收，确保自定义接收器正常工作
    parser.disableLibraryReceiver();
    // 重新初始化自定义接收器的中断处理程序
    receiver.init();
    
    // 设置设备名称
    const char* deviceNameStr = deviceName.c_str();
    if (deviceNameStr != nullptr) {
        strncpy(learnedDevice->deviceName, deviceNameStr, MAX_DEVICE_NAME_LEN - 1);
        learnedDevice->deviceName[MAX_DEVICE_NAME_LEN - 1] = '\0';
    } else {
        learnedDevice->deviceName[0] = '\0';
    }
    
    // 先统计命令数量，检查是否超过限制
    int commandCount = 0;
    if (commandNames.length() > 0) {
        int startPos = 0;
        int commaPos = commandNames.indexOf(',');
        while (commaPos >= 0) {
            String cmdName = commandNames.substring(startPos, commaPos);
            cmdName.trim();
            if (cmdName.length() > 0) {
                commandCount++;
            }
            startPos = commaPos + 1;
            commaPos = commandNames.indexOf(',', startPos);
        }
        // 处理最后一个命令
        if (startPos < commandNames.length()) {
            String cmdName = commandNames.substring(startPos);
            cmdName.trim();
            if (cmdName.length() > 0) {
                commandCount++;
            }
        }
    }
    
    // 检查指令数量是否超过限制
    if (commandCount > MAX_LEARN_COMMANDS) {
        Serial.println("错误：指令数量超过限制，最多支持 " + String(MAX_LEARN_COMMANDS) + " 个指令，当前有 " + String(commandCount) + " 个");
        return false;
    }
    
    // 解析指令名称列表
    parseCommandNames(commandNames);
    
    if (commandNamesCount == 0) {
        Serial.println("错误：没有有效的指令名称");
        return false;  // 没有有效的指令名称
    }
    
    // 初始化学习状态
    mode = LEARN_DEVICE;
    currentCommandIndex = 0;
    learnTimeoutMs = timeoutSeconds > 0 ? timeoutSeconds * 1000UL : DEFAULT_LEARN_TIMEOUT_SEC * 1000UL;
    lastSignalTime = millis();
    
    // 重置重复信号检测
    hasLastSignal = false;
    lastLearnedSignal.clear();
    lastLearnedSignalTime = 0;
    
    // 清空接收缓冲区
    receiver.clearBuffer();
    
    // 初始化指令数组
    for (int i = 0; i < MAX_LEARN_COMMANDS; i++) {
        learnedDevice->commands[i].valid = false;
        learnedDevice->commands[i].name[0] = '\0';
    }
    
    logger.logLearn("开始学习设备: " + deviceName + ", 指令数: " + String(commandNamesCount));
    Serial.println("=== 开始学习设备 ===");
    Serial.println("设备名称: " + deviceName);
    Serial.println("指令列表: " + commandNames);
    if (commandNamesCount > 0 && commandNamesList[0][0] != '\0') {
        Serial.println("请发送第1个指令: " + String(commandNamesList[0]));
    } else {
        Serial.println("请发送第1个指令");
    }
    
    return true;
}

bool IRLearner::stopDeviceLearn() {
    if (mode != LEARN_DEVICE || learnedDevice == nullptr) {
        return false;
    }
    
    // 标记学习完成（即使没有完成所有指令）
    learnedDevice->valid = (learnedDevice->commandCount > 0);
    
    mode = LEARN_NONE;
    
    // 重新启用IRremoteESP8266库的接收
    parser.enableLibraryReceiver();
    
    String result = learnedDevice->valid ? 
        ("学习完成，共学习 " + String(learnedDevice->commandCount) + " 个指令") :
        "学习已取消";
    
    logger.logLearn(result);
    Serial.println("=== " + result + " ===");
    
    // 清空重复信号检测
    hasLastSignal = false;
    lastLearnedSignal.clear();
    lastLearnedSignalTime = 0;
    
    // 注意：不立即释放内存，保留数据用于转换函数，在下次开始学习或手动释放时清理
    
    return true;
}

bool IRLearner::isLearningDevice() const {
    return mode == LEARN_DEVICE;
}

String IRLearner::getLearnProgress() const {
    if (mode != LEARN_DEVICE) {
        return "";
    }
    
    // 优化：使用reserve预分配内存，减少内存碎片
    String result;
    result.reserve(16);  // 预分配足够空间（如"5/10"）
    result = String(currentCommandIndex + 1);
    result += "/";
    result += String(commandNamesCount);
    return result;
}

bool IRLearner::getLearnedDevice(LearnedDevice& device) {
    if (learnedDevice == nullptr || !learnedDevice->valid) {
        return false;
    }
    
    device = *learnedDevice;
    return true;
}

String IRLearner::getCurrentCommandName() const {
    if (mode != LEARN_DEVICE) {
        return "";
    }
    
    if (currentCommandIndex >= 0 && currentCommandIndex < commandNamesCount && 
        currentCommandIndex < MAX_LEARN_COMMANDS) {
        return String(commandNamesList[currentCommandIndex]);
    }
    
    return "";
}

int IRLearner::getLearnedCommandCount() const {
    // 学习完成后，mode 变为 LEARN_NONE，但 learnedDevice 仍然有效
    // 所以需要检查 learnedDevice->valid 而不是 mode
    if (learnedDevice == nullptr || !learnedDevice->valid) {
        return 0;
    }
    
    return learnedDevice->commandCount;
}

int IRLearner::getTotalCommandCount() const {
    if (mode != LEARN_DEVICE) {
        return 0;
    }
    
    return commandNamesCount;
}

String IRLearner::getDeviceName() const {
    if (mode != LEARN_DEVICE || learnedDevice == nullptr) {
        return "";
    }
    
    return String(learnedDevice->deviceName);
}

void IRLearner::getLearnedCommandNames(String* names, int maxCount) const {
    if (names == nullptr || maxCount <= 0 || learnedDevice == nullptr) {
        return;
    }
    
    int count = 0;
    for (int i = 0; i < learnedDevice->commandCount && i < maxCount && i < MAX_LEARN_COMMANDS; i++) {
        if (learnedDevice->commands[i].valid) {
            names[count++] = String(learnedDevice->commands[i].name);
        }
    }
    
    // 填充剩余空间为空字符串
    for (int i = count; i < maxCount; i++) {
        names[i] = "";
    }
}

void IRLearner::getLearnedCommandNames(const char** names, int maxCount) const {
    if (names == nullptr || maxCount <= 0 || learnedDevice == nullptr) {
        return;
    }
    
    int count = 0;
    for (int i = 0; i < learnedDevice->commandCount && i < maxCount && i < MAX_LEARN_COMMANDS; i++) {
        if (learnedDevice->commands[i].valid) {
            names[count++] = learnedDevice->commands[i].name;
        }
    }
    
    // 填充剩余空间为nullptr
    for (int i = count; i < maxCount; i++) {
        names[i] = nullptr;
    }
}

void IRLearner::processDeviceLearnSignal(const IRTiming* timing, int count) {
    if (currentCommandIndex >= commandNamesCount || currentCommandIndex >= MAX_LEARN_COMMANDS) {
        return;  // 所有指令已学习完成
    }
    
    if (timing == nullptr || count <= 0) {
        Serial.println("[学习] 错误：时序数据无效");
        return;
    }
    
    // 验证信号长度
    const int MIN_VALID_TIMING_COUNT = 10;
    if (count < MIN_VALID_TIMING_COUNT) {
        Serial.println("错误：信号太短，无法学习");
        return;
    }
    
    // 检查信号是否超过最大长度限制
    int actualTimingCount = count;
    if (count > MAX_LEARN_TIMING_COUNT) {
        actualTimingCount = MAX_LEARN_TIMING_COUNT;
        Serial.printf("警告：信号长度 %d 超过最大限制 %d，将被截断\n", count, MAX_LEARN_TIMING_COUNT);
    }
    
    // 保存学习的指令
    if (currentCommandIndex >= MAX_LEARN_COMMANDS) {
        Serial.println("错误：指令索引超出范围");
        return;
    }
    
    if (learnedDevice == nullptr) {
        return;
    }
    
    LearnedCommand& cmd = learnedDevice->commands[currentCommandIndex];
    if (currentCommandIndex < commandNamesCount) {
        strncpy(cmd.name, commandNamesList[currentCommandIndex], MAX_COMMAND_NAME_LEN - 1);
        cmd.name[MAX_COMMAND_NAME_LEN - 1] = '\0';
    } else {
        cmd.name[0] = '\0';
    }
    
    // 尝试解析协议（可选，不影响学习结果）
    UnifiedSignal signal;
    bool parseSuccess = parser.parseSignal(timing, actualTimingCount, signal);
    
    if (!parseSuccess) {
        // 解析失败，使用时序码
        signal.clear();
        signal.useTiming = true;  // 时序码信号
        signal.timingBase64 = SignalUtils::timingToBase64(timing, actualTimingCount);
        signal.timingCount = actualTimingCount;
    } else {
        // 解析成功，保存UnifiedSignal
        // 如果解析成功但使用时序码，保存时序码Base64
        if (signal.useTiming) {
            signal.timingBase64 = SignalUtils::timingToBase64(timing, actualTimingCount);
        }
    }
    
    // 序列检测逻辑：判断是否加入当前序列
    unsigned long currentTime = millis();
    unsigned long interval = 0;
    if (hasLastSignal && lastLearnedSignalTime > 0) {
        if (currentTime >= lastLearnedSignalTime) {
            interval = currentTime - lastLearnedSignalTime;
        } else {
            interval = (0xFFFFFFFFUL - lastLearnedSignalTime) + currentTime;
        }
    }
    
    // 初始化序列（如果还没有）
    if (cmd.sequence.signalCount == 0) {
        cmd.sequence.clear();
        cmd.sequence.timestamp = currentTime;
    }
    
    // 在判断是否加入序列之前，先检查是否为重复码
    if (interval > 0 && interval < SIGNAL_INTERVAL_MAX_MS && cmd.sequence.signalCount > 0) {
        // 检查是否为重复码：信号相同且间隔在重复码范围内
        const UnifiedSignal& lastSignal = cmd.sequence.signals[cmd.sequence.signalCount - 1];
        if (SignalUtils::signalsEqual(signal, lastSignal) && 
            interval >= REPEAT_CODE_INTERVAL_MIN_MS && 
            interval <= REPEAT_CODE_INTERVAL_MAX_MS) {
            // 是重复码，增加上一个信号的repeatCount
            if (cmd.sequence.signals[cmd.sequence.signalCount - 1].repeatCount < SIGNAL_REPEAT_MAX) {
                cmd.sequence.signals[cmd.sequence.signalCount - 1].repeatCount++;
            }
            // 更新时间戳，但不添加新信号
            lastLearnedSignal = signal;
            hasLastSignal = true;
            lastLearnedSignalTime = currentTime;
            return;  // 直接返回，不继续处理
        }
    }
    
    // 判断是否加入序列
    if (interval > 0 && interval < SIGNAL_INTERVAL_MAX_MS && cmd.sequence.signalCount < MAX_B_SIGNALS_PER_RULE) {
        // 加入序列（标准模式：interval 是前一个信号和当前信号之间的间隔）
        cmd.sequence.addSignal(signal, interval, false);
    } else {
        // 开始新序列（单个信号也是序列）
        cmd.sequence.clear();
        cmd.sequence.timestamp = currentTime;
        cmd.sequence.addSignal(signal, 0, false);
    }
    
    // 重复信号检测
    if (hasLastSignal && SignalUtils::signalsEqual(signal, lastLearnedSignal)) {
        String signalDesc = parser.signalToString(signal);
        String logMsg = "检测到重复信号: " + signalDesc + "，间隔: " + String(interval) + "ms";
        logger.logLearn(logMsg);
        Serial.println("[学习] " + logMsg);
    }
    
    Serial.println("=== 指令学习完成 ===");
    Serial.println("指令名称: " + String(cmd.name));
    Serial.println("信号: " + parser.signalToString(signal));
    if (cmd.sequence.signalCount > 1) {
        Serial.printf("序列长度: %d 个信号\n", cmd.sequence.signalCount);
    }
    
    // 更新上一个学习的信号（无论是否重复都保存）
    lastLearnedSignal = signal;
    hasLastSignal = true;
    lastLearnedSignalTime = currentTime;
    
    cmd.valid = true;
    learnedDevice->commandCount = currentCommandIndex + 1;
    currentCommandIndex++;
    
    // 检查是否所有指令都已学习
    if (currentCommandIndex >= commandNamesCount) {
        // 所有指令学习完成
        learnedDevice->valid = true;
        mode = LEARN_NONE;
        // 重新启用IRremoteESP8266库的接收
        parser.enableLibraryReceiver();
        logger.logLearn("设备学习完成: " + String(learnedDevice->deviceName) + 
                       ", 共 " + String(learnedDevice->commandCount) + " 个指令");
        Serial.println("=== 所有指令学习完成 ===");
    } else {
        // 继续学习下一个指令
        lastSignalTime = millis();
        if (currentCommandIndex < commandNamesCount && currentCommandIndex < MAX_LEARN_COMMANDS) {
            Serial.println("请发送第" + String(currentCommandIndex + 1) + "个指令: " + 
                          String(commandNamesList[currentCommandIndex]));
        } else {
            Serial.println("请发送第" + String(currentCommandIndex + 1) + "个指令");
        }
    }
}

void IRLearner::checkLearnTimeout() {
    if (mode != LEARN_DEVICE) {
        return;
    }
    
    unsigned long now = millis();
    unsigned long elapsed = now - lastSignalTime;
    
    if (elapsed >= learnTimeoutMs) {
        // 超时，结束学习
        Serial.println("学习超时，自动结束学习");
        stopDeviceLearn();
    }
}

void IRLearner::parseCommandNames(const String& commandNamesStr) {
    commandNamesCount = 0;
    
    // 初始化数组，确保所有元素都是空字符串
    for (int i = 0; i < MAX_LEARN_COMMANDS; i++) {
        commandNamesList[i][0] = '\0';
    }
    
    if (commandNamesStr.length() == 0) {
        return;
    }
    
    int startPos = 0;
    int commaPos = commandNamesStr.indexOf(',');
    
    while (commaPos >= 0 && commandNamesCount < MAX_LEARN_COMMANDS) {
        String cmdName = commandNamesStr.substring(startPos, commaPos);
        cmdName.trim();
        if (cmdName.length() > 0 && cmdName.length() < MAX_COMMAND_NAME_LEN) {
            const char* cmdNameStr = cmdName.c_str();
            if (cmdNameStr != nullptr) {
                strncpy(commandNamesList[commandNamesCount], cmdNameStr, MAX_COMMAND_NAME_LEN - 1);
                commandNamesList[commandNamesCount][MAX_COMMAND_NAME_LEN - 1] = '\0';
                commandNamesCount++;
            }
        }
        startPos = commaPos + 1;
        commaPos = commandNamesStr.indexOf(',', startPos);
    }
    
    // 处理最后一个指令名称
    if (startPos < commandNamesStr.length() && commandNamesCount < MAX_LEARN_COMMANDS) {
        String cmdName = commandNamesStr.substring(startPos);
        cmdName.trim();
        if (cmdName.length() > 0 && cmdName.length() < MAX_COMMAND_NAME_LEN) {
            const char* cmdNameStr = cmdName.c_str();
            if (cmdNameStr != nullptr) {
                strncpy(commandNamesList[commandNamesCount], cmdNameStr, MAX_COMMAND_NAME_LEN - 1);
                commandNamesList[commandNamesCount][MAX_COMMAND_NAME_LEN - 1] = '\0';
                commandNamesCount++;
            }
        }
    }
}

void IRLearner::freeLearnedDevice() {
    if (learnedDevice != nullptr) {
        delete learnedDevice;
        learnedDevice = nullptr;
    }
}

// 辅助函数：将信号序列转换为字符串序列格式
static String sequenceToStringFormat(const SignalSequence& sequence) {
    String result;
    result.reserve(256);
    
    bool firstSignal = true;
    bool hasFirstInterval = (sequence.getIntervalCount() == sequence.signalCount);
    
    // 如果有第一个信号前的间隔
    if (hasFirstInterval && sequence.intervals[0] > 0) {
        result += "@";
        result += String(sequence.intervals[0]);
        result += ";";
    }
    
    // 遍历序列中的每个信号
    for (uint8_t j = 0; j < sequence.signalCount; j++) {
        const UnifiedSignal& signal = sequence.signals[j];
        
        if (!firstSignal) {
            result += ";";
        }
        firstSignal = false;
        
        // 转换信号为字符串格式
        if (signal.useTiming && signal.hasTiming()) {
            // 时序码信号：Base64格式
            result += signal.timingBase64;
        } else if (signal.isProtocol()) {
            // 协议信号：protocol,data格式
            result += String(signal.protocol);
            result += ",0x";
            char hexStr[9];
            snprintf(hexStr, sizeof(hexStr), "%08X", signal.data);
            result += hexStr;
        } else {
            // 无法转换，跳过
            continue;
        }
        
        // 添加间隔（如果有）
        uint16_t interval = 0;
        if (hasFirstInterval) {
            if (j < sequence.signalCount - 1) {
                interval = sequence.intervals[j + 1];
            }
        } else {
            if (j < sequence.signalCount - 1) {
                interval = sequence.intervals[j];
            }
        }
        
        if (interval > 0) {
            result += "@";
            result += String(interval);
        }
    }
    
    // 添加序列级别的repeatCount（如果>1）
    uint8_t seqRepeatCount = 1;
    if (sequence.signalCount > 0 && sequence.signals[0].repeatCount > 1) {
        seqRepeatCount = sequence.signals[0].repeatCount;
    }
    
    if (seqRepeatCount > 1) {
        result += "|";
        result += String(seqRepeatCount);
    }
    
    return result;
}

String IRLearner::convertToStringFormat() {
    if (learnedDevice == nullptr || !learnedDevice->valid || learnedDevice->commandCount == 0) {
        return "{}";
    }
    
    // 检查内存：如果内存不足，提前返回错误
    size_t freeHeap = ESP.getFreeHeap();
    size_t maxBlock = ESP.getMaxFreeBlockSize();
    
    // 估算JSON大小：基础结构 + 每个命令的字符串序列
    // 基础结构约200字节，每个命令的字符串序列平均100-200字节
    size_t estimatedSize = 256 + (learnedDevice->commandCount * 256);
    if (estimatedSize > 2048) estimatedSize = 2048;  // 限制最大2KB
    if (estimatedSize < 512) estimatedSize = 512;  // 至少512字节
    
    // 检查最大连续块是否足够
    size_t requiredMemory = estimatedSize + 512;  // 额外512字节用于字符串操作
    if (freeHeap < 1500 || maxBlock < requiredMemory) {
        Serial.printf("[IRLearner] convertToStringFormat: Insufficient memory (free: %d bytes, max block: %d bytes, required: %d bytes)\n", 
                     freeHeap, maxBlock, requiredMemory);
        return "{}";
    }
    
    DynamicJsonDocument doc(estimatedSize);
    
    // SmartIR JSON配置文件格式
    doc["manufacturer"] = "Learned";
    JsonArray models = doc.createNestedArray("supportedModels");
    models.add("Learned Device");
    doc["commandsEncoding"] = "Raw";
    doc["supportedController"] = "MQTT";
    
    // 创建commands对象
    JsonObject commands = doc.createNestedObject("commands");
    
    // 遍历所有命令，转换为字符串序列格式
    for (int i = 0; i < learnedDevice->commandCount && i < MAX_LEARN_COMMANDS; i++) {
        const LearnedCommand& cmd = learnedDevice->commands[i];
        if (!cmd.valid || cmd.sequence.signalCount == 0) {
            continue;
        }
        
        // 检查内存：如果内存不足，停止处理
        if (ESP.getFreeHeap() < 1500) {
            break;
        }
        
        // 转换序列为字符串格式
        String sequenceStr = sequenceToStringFormat(cmd.sequence);
        if (sequenceStr.length() > 0) {
            const char* cmdName = (cmd.name[0] != '\0') ? cmd.name : "";
            commands[cmdName] = sequenceStr;
        }
    }
    
    // 检查文档是否溢出
    if (doc.overflowed()) {
        char buffer[192];
        snprintf(buffer, sizeof(buffer), "[IRLearner: convertToStringFormat] JSON文档溢出，尝试使用更大的文档 (当前大小: %d bytes, 命令数: %d)", estimatedSize, learnedDevice->commandCount);
        logger.log("warning", buffer);
        
        // 尝试使用更大的文档重新构建
        size_t largerSize = estimatedSize * 2;
        if (largerSize <= 4096) {  // 限制最大4KB
            DynamicJsonDocument largerDoc(largerSize);
            largerDoc["manufacturer"] = "Learned";
            JsonArray largerModels = largerDoc.createNestedArray("supportedModels");
            largerModels.add("Learned Device");
            largerDoc["commandsEncoding"] = "Raw";
            largerDoc["supportedController"] = "MQTT";
            JsonObject largerCommands = largerDoc.createNestedObject("commands");
            
            for (int i = 0; i < learnedDevice->commandCount && i < MAX_LEARN_COMMANDS; i++) {
                const LearnedCommand& cmd = learnedDevice->commands[i];
                if (!cmd.valid || cmd.sequence.signalCount == 0) continue;
                if (ESP.getFreeHeap() < 1500) break;
                
                String sequenceStr = sequenceToStringFormat(cmd.sequence);
                if (sequenceStr.length() > 0) {
                    const char* cmdName = (cmd.name[0] != '\0') ? cmd.name : "";
                    largerCommands[cmdName] = sequenceStr;
                }
            }
            
            if (!largerDoc.overflowed()) {
                doc = largerDoc;
            } else {
                char errorBuffer[192];
                snprintf(errorBuffer, sizeof(errorBuffer), "[IRLearner: convertToStringFormat] 更大的文档仍然溢出 (大小: %d bytes, 命令数: %d)", largerSize, learnedDevice->commandCount);
                logger.log("error", errorBuffer);
            }
        } else {
            char errorBuffer[192];
            snprintf(errorBuffer, sizeof(errorBuffer), "[IRLearner: convertToStringFormat] JSON文档过大，无法重试 (需要: %d bytes, 命令数: %d)", largerSize, learnedDevice->commandCount);
            logger.log("error", errorBuffer);
        }
    }
    
    // 序列化为字符串
    String output;
    output.reserve(estimatedSize);  // 预分配内存
    serializeJson(doc, output);
    
    // 如果仍然溢出，记录错误
    if (doc.overflowed()) {
        char buffer[192];
        snprintf(buffer, sizeof(buffer), "[IRLearner: convertToStringFormat] JSON文档溢出，无法序列化 (大小: %d bytes, 命令数: %d)", estimatedSize, learnedDevice->commandCount);
        logger.log("error", buffer);
    }
    
    return output;
}

String IRLearner::convertToJSONFormat() {
    if (learnedDevice == nullptr || !learnedDevice->valid || learnedDevice->commandCount == 0) {
        return "{}";
    }
    
    // 检查内存：如果内存不足，提前返回错误
    size_t freeHeap = ESP.getFreeHeap();
    size_t maxBlock = ESP.getMaxFreeBlockSize();
    
    // 估算JSON大小：基础结构 + 每个命令的序列JSON对象
    // 基础结构约200字节，每个命令的序列JSON对象平均200-400字节
    size_t estimatedSize = 512 + (learnedDevice->commandCount * 512);
    if (estimatedSize > 2048) estimatedSize = 2048;  // 限制最大2KB
    if (estimatedSize < 1024) estimatedSize = 1024;  // 至少1KB
    
    // 检查最大连续块是否足够
    size_t requiredMemory = estimatedSize + 512;  // 额外512字节用于字符串操作
    if (freeHeap < 1500 || maxBlock < requiredMemory) {
        Serial.printf("[IRLearner] convertToJSONFormat: Insufficient memory (free: %d bytes, max block: %d bytes, required: %d bytes)\n", 
                     freeHeap, maxBlock, requiredMemory);
        return "{}";
    }
    
    DynamicJsonDocument doc(estimatedSize);
    
    // JSON格式（与SmartIR不兼容）
    doc["manufacturer"] = "Learned";
    JsonArray models = doc.createNestedArray("supportedModels");
    models.add("Learned Device");
    doc["commandsEncoding"] = "Raw";
    doc["supportedController"] = "MQTT";
    
    // 创建commands对象
    JsonObject commands = doc.createNestedObject("commands");
    
    // 遍历所有命令，转换为JSON对象（序列格式）
    for (int i = 0; i < learnedDevice->commandCount && i < MAX_LEARN_COMMANDS; i++) {
        const LearnedCommand& cmd = learnedDevice->commands[i];
        if (!cmd.valid || cmd.sequence.signalCount == 0) {
            continue;
        }
        
        // 检查内存：如果内存不足，停止处理
        if (ESP.getFreeHeap() < 1500) {
            break;
        }
        
        // 输出为JSON对象（序列格式）
        const char* cmdName = (cmd.name[0] != '\0') ? cmd.name : "";
        JsonObject seqObj = commands.createNestedObject(cmdName);
        SignalUtils::sequenceToJSON(cmd.sequence, seqObj, true);
    }
    
    // 检查文档是否溢出
    if (doc.overflowed()) {
        char buffer[192];
        snprintf(buffer, sizeof(buffer), "[IRLearner: convertToJSONFormat] JSON文档溢出，尝试使用更大的文档 (当前大小: %d bytes, 命令数: %d)", estimatedSize, learnedDevice->commandCount);
        logger.log("warning", buffer);
        
        // 尝试使用更大的文档重新构建
        size_t largerSize = estimatedSize * 2;
        if (largerSize <= 4096) {  // 限制最大4KB
            DynamicJsonDocument largerDoc(largerSize);
            largerDoc["manufacturer"] = "Learned";
            JsonArray largerModels = largerDoc.createNestedArray("supportedModels");
            largerModels.add("Learned Device");
            largerDoc["commandsEncoding"] = "Raw";
            largerDoc["supportedController"] = "MQTT";
            JsonObject largerCommands = largerDoc.createNestedObject("commands");
            
            for (int i = 0; i < learnedDevice->commandCount && i < MAX_LEARN_COMMANDS; i++) {
                const LearnedCommand& cmd = learnedDevice->commands[i];
                if (!cmd.valid || cmd.sequence.signalCount == 0) continue;
                if (ESP.getFreeHeap() < 1500) break;
                
                const char* cmdName = (cmd.name[0] != '\0') ? cmd.name : "";
                JsonObject seqObj = largerCommands.createNestedObject(cmdName);
                SignalUtils::sequenceToJSON(cmd.sequence, seqObj, true);
            }
            
            if (!largerDoc.overflowed()) {
                doc = largerDoc;
            } else {
                char errorBuffer[192];
                snprintf(errorBuffer, sizeof(errorBuffer), "[IRLearner: convertToJSONFormat] 更大的文档仍然溢出 (大小: %d bytes, 命令数: %d)", largerSize, learnedDevice->commandCount);
                logger.log("error", errorBuffer);
            }
        } else {
            char errorBuffer[192];
            snprintf(errorBuffer, sizeof(errorBuffer), "[IRLearner: convertToJSONFormat] JSON文档过大，无法重试 (需要: %d bytes, 命令数: %d)", largerSize, learnedDevice->commandCount);
            logger.log("error", errorBuffer);
        }
    }
    
    // 序列化为字符串
    String output;
    output.reserve(estimatedSize);  // 预分配内存
    serializeJson(doc, output);
    
    // 如果仍然溢出，记录错误
    if (doc.overflowed()) {
        char buffer[192];
        snprintf(buffer, sizeof(buffer), "[IRLearner: convertToJSONFormat] JSON文档溢出，无法序列化 (大小: %d bytes, 命令数: %d)", estimatedSize, learnedDevice->commandCount);
        logger.log("error", buffer);
    }
    
    return output;
}


// Base64解码 - 委托给 SignalUtils::base64ToTiming()
bool IRLearner::decodeBase64ToTiming(const String& base64, IRTiming* timing, int& count, int maxCount) {
    return SignalUtils::base64ToTiming(base64, timing, count, maxCount);
}

IRLearner::SignalFormat IRLearner::detectSignalFormat(const String& payload) {
    if (payload.length() == 0) {
        return FORMAT_BASE64;  // 默认
    }
    
    String trimmed = payload;
    trimmed.trim();
    
    // 检测是否为JSON格式（UnifiedSignal对象格式）
    if (trimmed.startsWith("{") && trimmed.endsWith("}")) {
        return FORMAT_SIGNALID;
    }
    
    // 检测是否为数组格式
    if (trimmed.startsWith("[") && trimmed.endsWith("]")) {
        return FORMAT_ARRAY;
    }
    
    // 检测是否为协议格式（包含逗号且是数字开头）
    if (trimmed.indexOf(',') > 0) {
        // 检查第一个字符是否为数字
        char firstChar = trimmed.charAt(0);
        if (firstChar >= '0' && firstChar <= '9') {
            return FORMAT_PROTOCOL;
        }
    }
    
    // 默认按Base64处理
    return FORMAT_BASE64;
}

bool IRLearner::parseArrayString(const String& arrayStr, IRTiming* timing, int& count, int maxCount) {
    if (arrayStr.length() == 0 || timing == nullptr || maxCount <= 0) {
        count = 0;
        return false;
    }
    
    String trimmed = arrayStr;
    trimmed.trim();
    
    // 移除方括号
    if (trimmed.startsWith("[")) {
        trimmed = trimmed.substring(1);
    }
    if (trimmed.endsWith("]")) {
        trimmed = trimmed.substring(0, trimmed.length() - 1);
    }
    
    // 解析逗号分隔的数字
    count = 0;
    int startPos = 0;
    bool level = true;  // 从高电平开始
    
    while (startPos < trimmed.length() && count < maxCount) {
        int commaPos = trimmed.indexOf(',', startPos);
        if (commaPos < 0) {
            commaPos = trimmed.length();
        }
        
        String numStr = trimmed.substring(startPos, commaPos);
        numStr.trim();
        
        if (numStr.length() > 0) {
            long duration = numStr.toInt();
            if (duration < 0) {
                duration = -duration;
                level = false;
            } else {
                level = true;
            }
            
            if (duration > 0 && duration < 1000000) {  // 合理的时序范围
                timing[count].duration = duration;
                timing[count].level = level;
                count++;
            }
        }
        
        startPos = commaPos + 1;
    }
    
    return (count > 0);
}

bool IRLearner::parseProtocolString(const String& protocolStr, UnifiedSignal& signal, uint8_t& repeatCount) {
    if (protocolStr.length() == 0) {
        return false;
    }
    
    String trimmed = protocolStr;
    trimmed.trim();
    
    // 按逗号分隔
    int comma1 = trimmed.indexOf(',');
    if (comma1 < 0) {
        return false;
    }
    
    // 检测逗号数量以判断格式
    int commaCount = 0;
    for (int i = 0; i < trimmed.length(); i++) {
        if (trimmed.charAt(i) == ',') {
            commaCount++;
        }
    }
    
    int comma2 = trimmed.indexOf(',', comma1 + 1);
    if (comma2 < 0) {
        // 只有两个参数：protocol,data（repeatCount默认为1）
        String protocolStr_part = trimmed.substring(0, comma1);
        String dataStr = trimmed.substring(comma1 + 1);
        
        protocolStr_part.trim();
        dataStr.trim();
        
        uint32_t protocol = protocolStr_part.toInt();
        uint32_t data = 0;
        
        // 解析十六进制数据
        if (dataStr.startsWith("0x") || dataStr.startsWith("0X")) {
            data = strtoul(dataStr.c_str(), nullptr, 16);
        } else {
            // 尝试十六进制（不带0x前缀）
            char* endPtr;
            data = strtoul(dataStr.c_str(), &endPtr, 16);
            // 如果解析失败或没有完全解析，尝试十进制
            if (*endPtr != '\0' || (data == 0 && dataStr.length() > 0 && dataStr.charAt(0) != '0')) {
                data = dataStr.toInt();
            }
        }
        
        signal.clear();
        signal.protocol = protocol;
        signal.data = data;
        signal.useTiming = false;  // 协议信号（protocol >= 1 && protocol <= 5 时）
        signal.repeatCount = 1;
        repeatCount = 1;
        
        return true;
    } else if (commaCount == 2) {
        // 三个参数：protocol,data,repeatCount（向后兼容）
        String protocolStr_part = trimmed.substring(0, comma1);
        String dataStr = trimmed.substring(comma1 + 1, comma2);
        String repeatStr = trimmed.substring(comma2 + 1);
        
        protocolStr_part.trim();
        dataStr.trim();
        repeatStr.trim();
        
        uint32_t protocol = protocolStr_part.toInt();
        uint32_t data = 0;
        
        // 解析十六进制数据
        if (dataStr.startsWith("0x") || dataStr.startsWith("0X")) {
            data = strtoul(dataStr.c_str(), nullptr, 16);
        } else {
            // 尝试十六进制（不带0x前缀）
            char* endPtr;
            data = strtoul(dataStr.c_str(), &endPtr, 16);
            // 如果解析失败或没有完全解析，尝试十进制
            if (*endPtr != '\0' || (data == 0 && dataStr.length() > 0 && dataStr.charAt(0) != '0')) {
                data = dataStr.toInt();
            }
        }
        
        uint8_t repeat = repeatStr.toInt();
        if (repeat == 0) repeat = 1;
        
        signal.clear();
        signal.protocol = protocol;
        signal.data = data;
        signal.useTiming = false;  // 协议信号（protocol >= 1 && protocol <= 5 时）
        signal.repeatCount = repeat;
        repeatCount = repeat;
        
        return true;
    } else if (commaCount == 4) {
        // 五个参数：protocol,data,hash,isProtocol,repeatCount（新格式）
        int comma3 = trimmed.indexOf(',', comma2 + 1);
        int comma4 = trimmed.indexOf(',', comma3 + 1);
        
        if (comma3 < 0 || comma4 < 0) {
            return false;
        }
        
        String protocolStr_part = trimmed.substring(0, comma1);
        String dataStr = trimmed.substring(comma1 + 1, comma2);
        String hashStr = trimmed.substring(comma2 + 1, comma3);
        String isProtocolStr = trimmed.substring(comma3 + 1, comma4);
        String repeatStr = trimmed.substring(comma4 + 1);
        
        protocolStr_part.trim();
        dataStr.trim();
        hashStr.trim();
        isProtocolStr.trim();
        repeatStr.trim();
        
        uint32_t protocol = protocolStr_part.toInt();
        uint32_t data = 0;
        
        // 解析data（十六进制）
        if (dataStr.startsWith("0x") || dataStr.startsWith("0X")) {
            data = strtoul(dataStr.c_str(), nullptr, 16);
        } else {
            char* endPtr;
            data = strtoul(dataStr.c_str(), &endPtr, 16);
            if (*endPtr != '\0' || (data == 0 && dataStr.length() > 0 && dataStr.charAt(0) != '0')) {
                data = dataStr.toInt();
            }
        }
        
        // hash字段已移除，不再解析（保留hashStr解析以兼容旧格式，但不使用）
        
        // 解析isProtocol（true/false或1/0，兼容旧格式）
        bool isProtocol = false;
        if (isProtocolStr == "true" || isProtocolStr == "1" || isProtocolStr == "True" || isProtocolStr == "TRUE") {
            isProtocol = true;
        }
        
        // 解析repeatCount
        uint8_t repeat = repeatStr.toInt();
        if (repeat == 0) repeat = 1;
        
        signal.clear();
        signal.protocol = protocol;
        signal.data = data;
        signal.useTiming = !isProtocol;  // 根据 isProtocol 设置 useTiming
        signal.repeatCount = repeat;
        repeatCount = repeat;
        
        return true;
    } else {
        // 不支持的参数数量
        return false;
    }
}

bool IRLearner::parseSignalPayload(const String& payload, UnifiedSignal& signal, IRTiming* timing, int& timingCount, int maxTimingCount, uint8_t& repeatCount, SignalFormat& format) {
    format = detectSignalFormat(payload);
    
    switch (format) {
        case FORMAT_ARRAY: {
            // 数组格式：支持|repeatCount后缀
            int pipePos = payload.lastIndexOf('|');
            if (pipePos > 0 && pipePos < payload.length() - 1) {
                // 有|后缀，解析repeatCount（使用atoi避免创建String对象）
                repeatCount = atoi(payload.c_str() + pipePos + 1);
                if (repeatCount == 0) repeatCount = 1;
                
                // 创建临时String用于数组解析
                String arrayPart = payload.substring(0, pipePos);
                if (!parseArrayString(arrayPart, timing, timingCount, maxTimingCount)) {
                    return false;
                }
            } else {
                // 无后缀，默认repeatCount=1
                if (!parseArrayString(payload, timing, timingCount, maxTimingCount)) {
                    return false;
                }
                repeatCount = 1;
            }
            return true;
        }
        
        case FORMAT_PROTOCOL: {
            // 协议格式
            if (!parseProtocolString(payload, signal, repeatCount)) {
                return false;
            }
            timingCount = 0;
            return true;
        }
        
        case FORMAT_BASE64: {
            // Base64格式：支持|repeatCount后缀
            int pipePos = payload.lastIndexOf('|');
            if (pipePos > 0 && pipePos < payload.length() - 1) {
                // 有|后缀，解析repeatCount（使用atoi避免创建String对象）
                repeatCount = atoi(payload.c_str() + pipePos + 1);
                if (repeatCount == 0) repeatCount = 1;
                
                // 创建临时String用于Base64解码
                String base64Part = payload.substring(0, pipePos);
                if (!decodeBase64ToTiming(base64Part, timing, timingCount, maxTimingCount)) {
                    return false;
                }
            } else {
                // 无后缀，默认repeatCount=1
                if (!decodeBase64ToTiming(payload, timing, timingCount, maxTimingCount)) {
                    return false;
                }
                repeatCount = 1;
            }
            return true;
        }
        
        case FORMAT_SIGNALID: {
            // UnifiedSignal对象格式（JSON），需要调用者解析JSON
            return false;  // 返回false，让调用者处理JSON解析
        }
        
        default:
            return false;
    }
}

bool IRLearner::parseStringSequence(const String& payload, SignalSequence& sequence, IRTiming* timingBuffer, int maxTimingCount) {
    if (payload.length() == 0 || timingBuffer == nullptr || maxTimingCount <= 0) {
        return false;
    }
    
    String trimmed = payload;
    trimmed.trim();
    
    if (trimmed.length() == 0) {
        return false;
    }
    
    sequence.clear();
    
    // 1. 先提取最后的|repeatCount（如果存在）
    uint8_t sequenceRepeatCount = 1;
    int pipePos = trimmed.lastIndexOf('|');
    if (pipePos > 0 && pipePos < trimmed.length() - 1) {
        // 有|后缀，解析repeatCount
        sequenceRepeatCount = atoi(trimmed.c_str() + pipePos + 1);
        if (sequenceRepeatCount == 0) sequenceRepeatCount = 1;
        if (sequenceRepeatCount > SIGNAL_REPEAT_MAX) sequenceRepeatCount = SIGNAL_REPEAT_MAX;
        
        // 移除|repeatCount部分
        trimmed = trimmed.substring(0, pipePos);
        trimmed.trim();
    }
    
    if (trimmed.length() == 0) {
        return false;
    }
    
    // 2. 检查第一个信号前是否有间隔（格式：@50;signal1@30;signal2）
    bool hasFirstInterval = false;
    uint16_t firstInterval = 0;
    int startPos = 0;
    
    if (trimmed.startsWith("@")) {
        // 第一个信号前有间隔
        hasFirstInterval = true;
        int firstSemicolon = trimmed.indexOf(';');
        if (firstSemicolon > 0) {
            // 提取@后面的数字（直到分号）
            String intervalStr = trimmed.substring(1, firstSemicolon);
            intervalStr.trim();
            firstInterval = intervalStr.toInt();
            
            // 从分号后开始解析信号
            startPos = firstSemicolon + 1;
        } else {
            // 格式错误：@50后面没有分号
            return false;
        }
    }
    
    // 3. 解析每个信号段（带 @ 前缀时，段尾 @ 表示「该段 IR 结束后的间隔」，写入 intervals[si+1]，与 intervals[0] 前导分开处理）
    uint16_t gapAfterSignal[MAX_B_SIGNALS_PER_RULE];
    for (int g = 0; g < MAX_B_SIGNALS_PER_RULE; g++) {
        gapAfterSignal[g] = 0;
    }
    
    int signalIndex = 0;
    int semicolonPos = trimmed.indexOf(';', startPos);
    
    while (startPos < trimmed.length() && signalIndex < MAX_B_SIGNALS_PER_RULE) {
        // 获取当前信号段
        String segment;
        if (semicolonPos >= 0) {
            segment = trimmed.substring(startPos, semicolonPos);
        } else {
            segment = trimmed.substring(startPos);
        }
        segment.trim();
        
        if (segment.length() == 0) {
            // 空段，跳过
            startPos = semicolonPos + 1;
            semicolonPos = trimmed.indexOf(';', startPos);
            continue;
        }
        
        // 提取@间隔（如果存在）
        uint16_t interval = 0;
        int atPos = segment.indexOf('@');
        String signalPart = segment;
        
        if (atPos > 0) {
            // 有@间隔
            String intervalStr = segment.substring(atPos + 1);
            intervalStr.trim();
            interval = intervalStr.toInt();
            signalPart = segment.substring(0, atPos);
            signalPart.trim();
        }
        
        if (signalPart.length() == 0) {
            // 只有间隔，没有信号，跳过
            startPos = semicolonPos + 1;
            semicolonPos = trimmed.indexOf(';', startPos);
            continue;
        }
        
        // 4. 解析信号部分（Base64/数组/协议格式）
        UnifiedSignal signal;
        signal.clear();
        signal.repeatCount = sequenceRepeatCount;  // 使用序列级别的repeatCount
        
        SignalFormat format = detectSignalFormat(signalPart);
        int timingCount = 0;
        uint8_t signalRepeatCount = 1;
        
        bool parseSuccess = false;
        
        switch (format) {
            case FORMAT_ARRAY: {
                // 数组格式
                if (parseArrayString(signalPart, timingBuffer, timingCount, maxTimingCount)) {
                    signal.useTiming = true;
                    signal.timingBase64 = SignalUtils::timingToBase64(timingBuffer, timingCount);
                    signal.timingCount = timingCount;
                    parseSuccess = true;
                }
                break;
            }
            
            case FORMAT_PROTOCOL: {
                // 协议格式
                if (parseProtocolString(signalPart, signal, signalRepeatCount)) {
                    signal.repeatCount = signalRepeatCount;  // 使用信号自身的repeatCount
                    parseSuccess = true;
                }
                break;
            }
            
            case FORMAT_BASE64: {
                // Base64格式
                if (decodeBase64ToTiming(signalPart, timingBuffer, timingCount, maxTimingCount)) {
                    signal.useTiming = true;
                    signal.timingBase64 = SignalUtils::timingToBase64(timingBuffer, timingCount);
                    signal.timingCount = timingCount;
                    parseSuccess = true;
                }
                break;
            }
            
            default:
                parseSuccess = false;
                break;
        }
        
        if (!parseSuccess) {
            // 解析失败，跳过这个信号
            startPos = semicolonPos + 1;
            semicolonPos = trimmed.indexOf(';', startPos);
            continue;
        }
        
        if (hasFirstInterval) {
            gapAfterSignal[signalIndex] = interval;
        }
        
        // 5. 添加到序列
        // 带 @ 前缀时：段内间隔不交给 addSignal（否则会与 intervals[0] 前导语义冲突并覆盖错误下标），在循环结束后写入 intervals[]。
        uint16_t passInterval = interval;
        bool useFirstIntervalLayout = false;
        if (!hasFirstInterval) {
            useFirstIntervalLayout = false;
        } else {
            passInterval = 0;
            useFirstIntervalLayout = true;
        }
        if (!sequence.addSignal(signal, passInterval, useFirstIntervalLayout)) {
            // 序列已满
            break;
        }
        
        signalIndex++;
        
        // 移动到下一个信号段
        if (semicolonPos >= 0) {
            startPos = semicolonPos + 1;
            semicolonPos = trimmed.indexOf(';', startPos);
        } else {
            break;
        }
    }
    
    if (sequence.signalCount == 0) {
        return false;
    }
    
    if (hasFirstInterval) {
        memset(sequence.intervals, 0, sizeof(sequence.intervals));
        sequence.intervals[0] = firstInterval;
        for (int si = 0; si < sequence.signalCount && si < MAX_B_SIGNALS_PER_RULE; si++) {
            if (gapAfterSignal[si] > 0 && (si + 1) < MAX_B_SIGNALS_PER_RULE) {
                sequence.intervals[si + 1] = gapAfterSignal[si];
            }
        }
    }
    
    sequence.valid = true;
    return true;
}
