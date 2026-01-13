#include "ir_learner.h"
#include "utils/hex_utils.h"
#include "unified_signal.h"
#include <ArduinoJson.h>

// Base64编码表
static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

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
        learnedDevice->commands[i].timingCount = 0;
        learnedDevice->commands[i].hasProtocol = false;
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
    
    // 总是保存时序数据
    cmd.timingCount = actualTimingCount;
    memcpy(cmd.timing, timing, sizeof(IRTiming) * cmd.timingCount);
    
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
    
    // 保存单个信号（用于向后兼容）
    cmd.signal = signal;
    cmd.hasProtocol = signal.isProtocol();
    
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
    
    // 判断是否加入序列
    if (interval > 0 && interval < SIGNAL_INTERVAL_MAX_MS && cmd.sequence.signalCount < MAX_B_SIGNALS_PER_RULE) {
        // 加入序列
        cmd.sequence.addSignal(signal, interval);
    } else {
        // 开始新序列（单个信号也是序列）
        cmd.sequence.clear();
        cmd.sequence.timestamp = currentTime;
        cmd.sequence.addSignal(signal, 0);
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

String IRLearner::convertToBase64Format() {
    if (learnedDevice == nullptr || !learnedDevice->valid || learnedDevice->commandCount == 0) {
        return "{}";
    }
    
    // 检查内存：如果内存不足，提前返回错误
    size_t freeHeap = ESP.getFreeHeap();
    size_t maxBlock = ESP.getMaxFreeBlockSize();
    
    // 动态计算所需内存：基础JSON结构 + 每个命令的Base64字符串
    // 基础结构约200字节，每个命令的Base64字符串平均200-400字节
    size_t estimatedSize = 512 + (learnedDevice->commandCount * 512);
    if (estimatedSize > 3072) estimatedSize = 3072;  // 限制最大3KB
    if (estimatedSize < 1024) estimatedSize = 1024;  // 至少1KB
    
    // 需要至少：估算大小 + 1KB安全余量，但不超过可用内存的80%
    size_t requiredMemory = estimatedSize + 1024;
    size_t maxAllowedMemory = (freeHeap * 80) / 100;  // 最多使用80%的可用内存
    if (requiredMemory > maxAllowedMemory) {
        requiredMemory = maxAllowedMemory;
    }
    
    // 检查最大连续块是否足够
    if (freeHeap < 2048 || maxBlock < estimatedSize) {
        Serial.printf("[IRLearner] convertToBase64Format: Insufficient memory (free: %d bytes, max block: %d bytes, required: %d bytes)\n", 
                     freeHeap, maxBlock, estimatedSize);
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
    
    // 遍历所有命令，转换为Base64
    for (int i = 0; i < learnedDevice->commandCount && i < MAX_LEARN_COMMANDS; i++) {
        const LearnedCommand& cmd = learnedDevice->commands[i];
        if (!cmd.valid) {
            continue;
        }
        
        // 检查内存：如果内存不足，停止处理
        if (ESP.getFreeHeap() < 2048) {
            Serial.println("[IRLearner] convertToBase64Format: Memory low during processing, stopping");
            break;
        }
        
        // 编码Base64
        String base64 = encodeTimingToBase64(cmd.timing, cmd.timingCount);
        if (base64.length() == 0) {
            continue;  // 编码失败，跳过
        }
        
        // 添加到JSON对象（直接是Base64字符串，不是JSON包装）
        const char* cmdName = (cmd.name[0] != '\0') ? cmd.name : "";
        commands[cmdName] = base64;
    }
    
    // 检查文档是否溢出
    if (doc.overflowed()) {
        char buffer[192];
        snprintf(buffer, sizeof(buffer), "[IRLearner: convertToBase64Format] JSON文档溢出，尝试使用更大的文档 (当前大小: %d bytes, 命令数: %d)", estimatedSize, learnedDevice->commandCount);
        logger.log("warning", buffer);
        
        // 尝试使用更大的文档重新构建
        size_t largerSize = estimatedSize * 2;
        if (largerSize <= 6144) {  // 限制最大6KB
            DynamicJsonDocument largerDoc(largerSize);
            largerDoc["manufacturer"] = "Learned";
            JsonArray largerModels = largerDoc.createNestedArray("supportedModels");
            largerModels.add("Learned Device");
            largerDoc["commandsEncoding"] = "Raw";
            largerDoc["supportedController"] = "MQTT";
            JsonObject largerCommands = largerDoc.createNestedObject("commands");
            
            for (int i = 0; i < learnedDevice->commandCount && i < MAX_LEARN_COMMANDS; i++) {
                const LearnedCommand& cmd = learnedDevice->commands[i];
                if (!cmd.valid) continue;
                if (ESP.getFreeHeap() < 2048) break;
                
                String base64 = encodeTimingToBase64(cmd.timing, cmd.timingCount);
                if (base64.length() == 0) continue;
                
                const char* cmdName = (cmd.name[0] != '\0') ? cmd.name : "";
                largerCommands[cmdName] = base64;
            }
            
            if (!largerDoc.overflowed()) {
                doc = largerDoc;
            } else {
                char errorBuffer[192];
                snprintf(errorBuffer, sizeof(errorBuffer), "[IRLearner: convertToBase64Format] 更大的文档仍然溢出 (大小: %d bytes, 命令数: %d)", largerSize, learnedDevice->commandCount);
                logger.log("error", errorBuffer);
            }
        } else {
            char errorBuffer[192];
            snprintf(errorBuffer, sizeof(errorBuffer), "[IRLearner: convertToBase64Format] JSON文档过大，无法重试 (需要: %d bytes, 命令数: %d)", largerSize, learnedDevice->commandCount);
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
        snprintf(buffer, sizeof(buffer), "[IRLearner: convertToBase64Format] JSON文档溢出，无法序列化 (大小: %d bytes, 命令数: %d)", estimatedSize, learnedDevice->commandCount);
        logger.log("error", buffer);
    }
    
    return output;
}

String IRLearner::convertToSignalIDFormat() {
    if (learnedDevice == nullptr || !learnedDevice->valid || learnedDevice->commandCount == 0) {
        return "{}";
    }
    
    // 检查内存：如果内存不足，提前返回错误
    size_t freeHeap = ESP.getFreeHeap();
    size_t maxBlock = ESP.getMaxFreeBlockSize();
    
    // 估算JSON大小：基础结构 + 每个命令的平均大小
    // 基础结构约200字节，每个命令平均50-100字节（协议格式字符串较短）
    size_t estimatedSize = 256 + (learnedDevice->commandCount * 128);
    if (estimatedSize > 2048) estimatedSize = 2048;  // 限制最大2KB
    if (estimatedSize < 512) estimatedSize = 512;  // 至少512字节
    
    // 检查最大连续块是否足够（需要额外空间用于字符串操作）
    size_t requiredMemory = estimatedSize + 512;  // 额外512字节用于字符串操作
    if (freeHeap < 1500 || maxBlock < requiredMemory) {
        Serial.printf("[IRLearner] convertToSignalIDFormat: Insufficient memory (free: %d bytes, max block: %d bytes, required: %d bytes)\n", 
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
    
    int protocolCount = 0;
    int base64Count = 0;
    
    // 复用单个JSON文档用于SignalID对象（避免在循环中创建多个文档）
    DynamicJsonDocument signalDoc(256);
    
    // 遍历所有命令
    for (int i = 0; i < learnedDevice->commandCount && i < MAX_LEARN_COMMANDS; i++) {
        const LearnedCommand& cmd = learnedDevice->commands[i];
        if (!cmd.valid) {
            continue;
        }
        
        const char* cmdName = (cmd.name[0] != '\0') ? cmd.name : "";
        
        // 检查内存：如果内存不足，停止处理
        if (ESP.getFreeHeap() < 1500) {
            break;
        }
        
        // 使用序列格式输出（单个信号也是序列）
        if (cmd.sequence.signalCount > 0 && cmd.sequence.valid) {
            // 序列格式：输出为JSON对象
            JsonObject seqObj = commands.createNestedObject(cmdName);
            SignalUtils::sequenceToJSON(cmd.sequence, seqObj, true);
            protocolCount++;
        } else {
            // 回退：如果序列无效，使用单个信号格式
            if (cmd.hasProtocol && !cmd.signal.useTiming) {
                // 协议格式字符串
                char protocolStr[128];
                snprintf(protocolStr, sizeof(protocolStr), "%u,0x%08X,%u", 
                         cmd.signal.protocol, cmd.signal.data, cmd.signal.repeatCount);
                commands[cmdName] = protocolStr;
                protocolCount++;
            } else if (cmd.signal.useTiming) {
                // 时序码Base64格式
                commands[cmdName] = cmd.signal.timingBase64;
                base64Count++;
            } else {
                // 回退到Base64格式
                String base64 = encodeTimingToBase64(cmd.timing, cmd.timingCount);
                if (base64.length() > 0) {
                    commands[cmdName] = base64;
                    base64Count++;
                }
            }
        }
    }
    
    // 检查文档是否溢出
    if (doc.overflowed()) {
        char buffer[192];
        snprintf(buffer, sizeof(buffer), "[IRLearner: convertToSignalIDFormat] JSON文档溢出，尝试使用更大的文档 (当前大小: %d bytes, 命令数: %d)", estimatedSize, learnedDevice->commandCount);
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
                if (!cmd.valid) continue;
                if (ESP.getFreeHeap() < 1500) break;
                
                const char* cmdName = (cmd.name[0] != '\0') ? cmd.name : "";
                
                // 使用序列格式输出
                if (cmd.sequence.signalCount > 0 && cmd.sequence.valid) {
                    JsonObject seqObj = largerCommands.createNestedObject(cmdName);
                    SignalUtils::sequenceToJSON(cmd.sequence, seqObj, true);
                } else {
                    // 回退：单个信号格式
                    if (cmd.hasProtocol && !cmd.signal.useTiming) {
                        char protocolStr[128];
                        snprintf(protocolStr, sizeof(protocolStr), "%u,0x%08X,%u", 
                                 cmd.signal.protocol, cmd.signal.data, cmd.signal.repeatCount);
                        largerCommands[cmdName] = protocolStr;
                    } else if (cmd.signal.useTiming) {
                        largerCommands[cmdName] = cmd.signal.timingBase64;
                    } else {
                        String base64 = encodeTimingToBase64(cmd.timing, cmd.timingCount);
                        if (base64.length() > 0) {
                            largerCommands[cmdName] = base64;
                        }
                    }
                }
            }
            
            if (!largerDoc.overflowed()) {
                doc = largerDoc;
            } else {
                char errorBuffer[192];
                snprintf(errorBuffer, sizeof(errorBuffer), "[IRLearner: convertToSignalIDFormat] 更大的文档仍然溢出 (大小: %d bytes, 命令数: %d)", largerSize, learnedDevice->commandCount);
                logger.log("error", errorBuffer);
            }
        } else {
            char errorBuffer[192];
            snprintf(errorBuffer, sizeof(errorBuffer), "[IRLearner: convertToSignalIDFormat] JSON文档过大，无法重试 (需要: %d bytes, 命令数: %d)", largerSize, learnedDevice->commandCount);
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
        snprintf(buffer, sizeof(buffer), "[IRLearner: convertToSignalIDFormat] JSON文档溢出，无法序列化 (大小: %d bytes, 命令数: %d)", estimatedSize, learnedDevice->commandCount);
        logger.log("error", buffer);
    }
    
    return output;
}

String IRLearner::convertToArrayFormat() {
    if (learnedDevice == nullptr || !learnedDevice->valid || learnedDevice->commandCount == 0) {
        return "{}";
    }
    
    // 检查内存：如果内存不足，提前返回错误
    size_t freeHeap = ESP.getFreeHeap();
    size_t maxBlock = ESP.getMaxFreeBlockSize();
    
    // 估算JSON大小：基础结构 + 每个命令的数组字符串
    // 基础结构约200字节，每个命令的数组字符串平均200-400字节
    size_t estimatedSize = 512 + (learnedDevice->commandCount * 512);
    if (estimatedSize > 3072) estimatedSize = 3072;  // 限制最大3KB
    if (estimatedSize < 1024) estimatedSize = 1024;  // 至少1KB
    
    // 检查最大连续块是否足够
    if (freeHeap < 2048 || maxBlock < estimatedSize) {
        Serial.printf("[IRLearner] convertToArrayFormat: Insufficient memory (free: %d bytes, max block: %d bytes, required: %d bytes)\n", 
                     freeHeap, maxBlock, estimatedSize);
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
    
    // 遍历所有命令，转换为数组格式
    for (int i = 0; i < learnedDevice->commandCount && i < MAX_LEARN_COMMANDS; i++) {
        const LearnedCommand& cmd = learnedDevice->commands[i];
        if (!cmd.valid) {
            continue;
        }
        
        // 检查内存：如果内存不足，停止处理
        if (ESP.getFreeHeap() < 2048) {
            Serial.println("[IRLearner] convertToArrayFormat: Memory low during processing, stopping");
            break;
        }
        
        // 使用预分配的String，减少内存碎片
        String arrayStr;
        // 估算数组字符串大小：每个数字平均8字节（包括逗号）
        arrayStr.reserve(cmd.timingCount * 10 + 2);  // +2 for brackets
        
        arrayStr = "[";
        for (int j = 0; j < cmd.timingCount; j++) {
            if (j > 0) arrayStr += ",";
            // 正数表示高电平，负数表示低电平
            long duration = cmd.timing[j].duration;
            if (!cmd.timing[j].level) {
                duration = -duration;
            }
            arrayStr += String(duration);
        }
        arrayStr += "]";
        
        // 添加到JSON对象（直接是数组字符串）
        const char* cmdName = (cmd.name[0] != '\0') ? cmd.name : "";
        commands[cmdName] = arrayStr;
    }
    
    // 检查文档是否溢出
    if (doc.overflowed()) {
        char buffer[192];
        snprintf(buffer, sizeof(buffer), "[IRLearner: convertToArrayFormat] JSON文档溢出，尝试使用更大的文档 (当前大小: %d bytes, 命令数: %d)", estimatedSize, learnedDevice->commandCount);
        logger.log("warning", buffer);
        
        // 尝试使用更大的文档重新构建
        size_t largerSize = estimatedSize * 2;
        if (largerSize <= 6144) {  // 限制最大6KB
            DynamicJsonDocument largerDoc(largerSize);
            largerDoc["manufacturer"] = "Learned";
            JsonArray largerModels = largerDoc.createNestedArray("supportedModels");
            largerModels.add("Learned Device");
            largerDoc["commandsEncoding"] = "Raw";
            largerDoc["supportedController"] = "MQTT";
            JsonObject largerCommands = largerDoc.createNestedObject("commands");
            
            for (int i = 0; i < learnedDevice->commandCount && i < MAX_LEARN_COMMANDS; i++) {
                const LearnedCommand& cmd = learnedDevice->commands[i];
                if (!cmd.valid) continue;
                if (ESP.getFreeHeap() < 2048) break;
                
                String arrayStr;
                arrayStr.reserve(cmd.timingCount * 10 + 2);
                arrayStr = "[";
                for (int j = 0; j < cmd.timingCount; j++) {
                    if (j > 0) arrayStr += ",";
                    long duration = cmd.timing[j].duration;
                    if (!cmd.timing[j].level) {
                        duration = -duration;
                    }
                    arrayStr += String(duration);
                }
                arrayStr += "]";
                
                const char* cmdName = (cmd.name[0] != '\0') ? cmd.name : "";
                largerCommands[cmdName] = arrayStr;
            }
            
            if (!largerDoc.overflowed()) {
                doc = largerDoc;
            } else {
                char errorBuffer[192];
                snprintf(errorBuffer, sizeof(errorBuffer), "[IRLearner: convertToArrayFormat] 更大的文档仍然溢出 (大小: %d bytes, 命令数: %d)", largerSize, learnedDevice->commandCount);
                logger.log("error", errorBuffer);
            }
        } else {
            char errorBuffer[192];
            snprintf(errorBuffer, sizeof(errorBuffer), "[IRLearner: convertToArrayFormat] JSON文档过大，无法重试 (需要: %d bytes, 命令数: %d)", largerSize, learnedDevice->commandCount);
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
        snprintf(buffer, sizeof(buffer), "[IRLearner: convertToArrayFormat] JSON文档溢出，无法序列化 (大小: %d bytes, 命令数: %d)", estimatedSize, learnedDevice->commandCount);
        logger.log("error", buffer);
    }
    
    return output;
}

String IRLearner::encodeTimingToBase64(const IRTiming* timing, int count) {
    if (timing == nullptr || count == 0) {
        return "";
    }
    
    // 限制最大时序点数
    int maxCount = (count > MAX_LEARN_TIMING_COUNT) ? MAX_LEARN_TIMING_COUNT : count;
    int byteCount = maxCount * 2;  // 每个时序点2字节（持续时间）
    
    // 使用固定大小的栈缓冲区（400字节足够）
    if (byteCount > 400) {
        return "";  // 信号太长，无法处理
    }
    
    uint8_t bytes[400];  // 固定栈缓冲区
    
    // 将时序数据转换为字节数组（持续时间以10μs为单位）
    for (int i = 0; i < maxCount; i++) {
        uint16_t duration10us = (uint16_t)(timing[i].duration / 10);
        bytes[i * 2] = (uint8_t)(duration10us & 0xFF);
        bytes[i * 2 + 1] = (uint8_t)((duration10us >> 8) & 0xFF);
    }
    
    // Base64编码
    String base64;
    base64.reserve((byteCount * 4 / 3) + 4);  // 预分配空间
    
    int i = 0;
    int j = 0;
    uint8_t char_array_3[3];
    uint8_t char_array_4[4];
    
    while (i < byteCount) {
        char_array_3[j++] = bytes[i++];
        if (j == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            
            for (j = 0; j < 4; j++) {
                base64 += base64_chars[char_array_4[j]];
            }
            j = 0;
        }
    }
    
    // 处理剩余字节
    if (j > 0) {
        for (int k = j; k < 3; k++) {
            char_array_3[k] = '\0';
        }
        
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;
        
        for (int k = 0; k < j + 1; k++) {
            base64 += base64_chars[char_array_4[k]];
        }
        
        while (j++ < 3) {
            base64 += '=';
        }
    }
    
    return base64;
}


bool IRLearner::decodeBase64ToTiming(const String& base64, IRTiming* timing, int& count, int maxCount) {
    if (base64.length() == 0 || timing == nullptr || maxCount <= 0) {
        count = 0;
        return false;
    }
    
    // Base64解码：计算解码后的字节数
    int paddingCount = 0;
    if (base64.length() > 0 && base64[base64.length() - 1] == '=') {
        paddingCount++;
        if (base64.length() > 1 && base64[base64.length() - 2] == '=') {
            paddingCount++;
        }
    }
    
    int base64Len = base64.length() - paddingCount;
    int byteCount = (base64Len * 3 / 4);
    
    // 检查解码后的字节数是否合理（每个时序点2字节）
    if (byteCount % 2 != 0 || byteCount == 0) {
        count = 0;
        return false;
    }
    
    int timingCount = byteCount / 2;
    if (timingCount > maxCount) {
        timingCount = maxCount;
        byteCount = timingCount * 2;
    }
    
    // 检查可用内存
    size_t requiredMemory = byteCount + 1024;
    if (ESP.getFreeHeap() < requiredMemory) {
        count = 0;
        return false;
    }
    
    // 根据数据大小选择栈缓冲区或动态分配
    uint8_t* bytes = nullptr;
    bool useStackBuffer = false;
    uint8_t stackBuffer[400];
    
    if (byteCount <= 400) {
        bytes = stackBuffer;
        useStackBuffer = true;
    } else {
        size_t maxFreeBlock = ESP.getMaxFreeBlockSize();
        if (maxFreeBlock < byteCount + 512) {
            count = 0;
            return false;
        }
        
        bytes = new uint8_t[byteCount];
        if (bytes == nullptr) {
            count = 0;
            return false;
        }
        useStackBuffer = false;
    }
    
    // Base64解码
    int i = 0;
    int j = 0;
    uint8_t char_array_4[4];
    uint8_t char_array_3[3];
    
    // Base64字符到值的映射函数
    auto base64CharToValue = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    
    for (int idx = 0; idx < base64Len && j < byteCount; idx++) {
        char c = base64[idx];
        if (c == '=') break;
        
        int value = base64CharToValue(c);
        if (value < 0) {
            if (!useStackBuffer && bytes != nullptr) {
                delete[] bytes;
            }
            count = 0;
            return false;
        }
        
        char_array_4[i++] = value;
        
        if (i == 4) {
            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
            
            for (int k = 0; k < 3 && j < byteCount; k++) {
                bytes[j++] = char_array_3[k];
            }
            i = 0;
        }
    }
    
    // 处理剩余的Base64字符
    if (i > 0) {
        for (int k = i; k < 4; k++) {
            char_array_4[k] = 0;
        }
        
        char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
        char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
        
        for (int k = 0; k < i - 1 && j < byteCount; k++) {
            bytes[j++] = char_array_3[k];
        }
    }
    
    // 将字节数组转换为IRTiming数组
    count = timingCount;
    bool level = true;
    
    for (int idx = 0; idx < timingCount; idx++) {
        uint16_t duration10us = bytes[idx * 2] | (bytes[idx * 2 + 1] << 8);
        timing[idx].duration = duration10us * 10;
        timing[idx].level = level;
        level = !level;
    }
    
    // 释放动态分配的内存
    if (!useStackBuffer && bytes != nullptr) {
        delete[] bytes;
    }
    
    return (count > 0);
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
