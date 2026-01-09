#include "ir_learner.h"

// Base64编码表
static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

IRLearner::IRLearner(IRReceiver& receiver, IRParser& parser, Logger& logger)
    : receiver(receiver), parser(parser), logger(logger),
      mode(LEARN_NONE),
      currentCommandIndex(0), commandNamesCount(0), lastSignalTime(0), learnTimeoutMs(0) {
    learnedDevice.valid = false;
    learnedDevice.commandCount = 0;
    learnedDevice.deviceName[0] = '\0';
}

void IRLearner::init() {
    mode = LEARN_NONE;
    learnedDevice.valid = false;
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
    
    // 禁用IRremoteESP8266库的接收，确保自定义接收器正常工作
    parser.disableLibraryReceiver();
    // 重新初始化自定义接收器的中断处理程序
    receiver.init();
    
    // 清空之前的学习数据
    learnedDevice.valid = false;
    learnedDevice.commandCount = 0;
    learnedDevice.deviceName[0] = '\0';
    
    // 设置设备名称
    const char* deviceNameStr = deviceName.c_str();
    if (deviceNameStr != nullptr) {
        strncpy(learnedDevice.deviceName, deviceNameStr, MAX_DEVICE_NAME_LEN - 1);
        learnedDevice.deviceName[MAX_DEVICE_NAME_LEN - 1] = '\0';
    } else {
        learnedDevice.deviceName[0] = '\0';
    }
    
    // 解析指令名称列表
    parseCommandNames(commandNames);
    
    if (commandNamesCount == 0) {
        Serial.println("错误：没有有效的指令名称");
        return false;  // 没有有效的指令名称
    }
    
    // 检查指令数量是否超过限制
    if (commandNamesCount > MAX_LEARN_COMMANDS) {
        Serial.println("错误：指令数量超过限制，最多支持 " + String(MAX_LEARN_COMMANDS) + " 个指令");
        return false;
    }
    
    // 初始化学习状态
    mode = LEARN_DEVICE;
    currentCommandIndex = 0;
    learnTimeoutMs = timeoutSeconds > 0 ? timeoutSeconds * 1000UL : DEFAULT_LEARN_TIMEOUT_SEC * 1000UL;
    lastSignalTime = millis();
    
    // 清空接收缓冲区
    receiver.clearBuffer();
    
    // 初始化指令数组
    for (int i = 0; i < MAX_LEARN_COMMANDS; i++) {
        learnedDevice.commands[i].valid = false;
        learnedDevice.commands[i].timingCount = 0;
        learnedDevice.commands[i].name[0] = '\0';
    }
    
    learnedDevice.learnTime = millis();
    
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
    if (mode != LEARN_DEVICE) {
        return false;
    }
    
    // 标记学习完成（即使没有完成所有指令）
    learnedDevice.valid = (learnedDevice.commandCount > 0);
    
    mode = LEARN_NONE;
    
    // 重新启用IRremoteESP8266库的接收
    parser.enableLibraryReceiver();
    
    String result = learnedDevice.valid ? 
        ("学习完成，共学习 " + String(learnedDevice.commandCount) + " 个指令") :
        "学习已取消";
    
    logger.logLearn(result);
    Serial.println("=== " + result + " ===");
    
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
    if (!learnedDevice.valid) {
        return false;
    }
    
    device = learnedDevice;
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
    if (mode != LEARN_DEVICE) {
        return 0;
    }
    
    return learnedDevice.commandCount;
}

int IRLearner::getTotalCommandCount() const {
    if (mode != LEARN_DEVICE) {
        return 0;
    }
    
    return commandNamesCount;
}

String IRLearner::getDeviceName() const {
    if (mode != LEARN_DEVICE) {
        return "";
    }
    
    return String(learnedDevice.deviceName);
}

void IRLearner::getLearnedCommandNames(String* names, int maxCount) const {
    if (names == nullptr || maxCount <= 0) {
        return;
    }
    
    int count = 0;
    for (int i = 0; i < learnedDevice.commandCount && i < maxCount && i < MAX_LEARN_COMMANDS; i++) {
        if (learnedDevice.commands[i].valid) {
            names[count++] = String(learnedDevice.commands[i].name);
        }
    }
    
    // 填充剩余空间为空字符串
    for (int i = count; i < maxCount; i++) {
        names[i] = "";
    }
}

void IRLearner::getLearnedCommandNames(const char** names, int maxCount) const {
    if (names == nullptr || maxCount <= 0) {
        return;
    }
    
    int count = 0;
    for (int i = 0; i < learnedDevice.commandCount && i < maxCount && i < MAX_LEARN_COMMANDS; i++) {
        if (learnedDevice.commands[i].valid) {
            names[count++] = learnedDevice.commands[i].name;
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
    
    // 解析信号
    SignalID signalId;
    if (!parser.parseSignal(timing, count, signalId)) {
        Serial.println("信号解析失败，请重试");
        return;
    }
    
    // 保存学习的指令
    if (currentCommandIndex >= MAX_LEARN_COMMANDS) {
        Serial.println("错误：指令索引超出范围");
        return;
    }
    
    LearnedCommand& cmd = learnedDevice.commands[currentCommandIndex];
    if (currentCommandIndex < commandNamesCount) {
        strncpy(cmd.name, commandNamesList[currentCommandIndex], MAX_COMMAND_NAME_LEN - 1);
        cmd.name[MAX_COMMAND_NAME_LEN - 1] = '\0';
    } else {
        cmd.name[0] = '\0';
    }
    cmd.signalId = signalId;
    cmd.timingCount = (count <= MAX_LEARN_TIMING_COUNT) ? count : MAX_LEARN_TIMING_COUNT;
    memcpy(cmd.timing, timing, sizeof(IRTiming) * cmd.timingCount);
    cmd.valid = true;
    
    learnedDevice.commandCount = currentCommandIndex + 1;
    currentCommandIndex++;
    
    Serial.println("=== 指令学习完成 ===");
    Serial.println("指令名称: " + String(cmd.name));
    Serial.println("信号: " + parser.signalToString(signalId));
    
    // 检查是否所有指令都已学习
    if (currentCommandIndex >= commandNamesCount) {
        // 所有指令学习完成
        learnedDevice.valid = true;
        mode = LEARN_NONE;
        // 重新启用IRremoteESP8266库的接收
        parser.enableLibraryReceiver();
        logger.logLearn("设备学习完成: " + String(learnedDevice.deviceName) + 
                       ", 共 " + String(learnedDevice.commandCount) + " 个指令");
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

String IRLearner::convertToSmartIRFormat() {
    if (!learnedDevice.valid || learnedDevice.commandCount == 0) {
        return "{}";
    }
    
    // 首先检查内存，确保有足够空间
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 10240) {  // 至少需要10KB空闲内存
        return "{}";  // 内存不足，返回空JSON
    }
    
    // 估算所需内存：Base64编码后的字符串大小约为原始数据的4/3
    // 每个命令：200个时序点 * 2字节 = 400字节，Base64后约533字节
    // 加上JSON结构，每个命令约650字节（增加估算值）
    size_t estimatedSize = 200 + (learnedDevice.commandCount * 650);
    
    // 检查内存是否足够（大幅增加安全余量）
    if (freeHeap < estimatedSize + 4096) {
        return "{}";  // 内存不足，返回空JSON
    }
    
    // 使用reserve预分配内存，减少内存重新分配
    String json;
    if (!json.reserve(estimatedSize)) {
        // reserve失败，返回空JSON
        return "{}";
    }
    
    // 安全地构建JSON字符串
    json = "{";
    json += "\"deviceName\":\"";
    
    // 确保deviceName是有效的字符串
    if (learnedDevice.deviceName[0] != '\0') {
        json += learnedDevice.deviceName;
    }
    json += "\",\"commands\":{";
    
    bool firstCommand = true;
    int validCommandCount = 0;
    
    for (int i = 0; i < learnedDevice.commandCount && i < MAX_LEARN_COMMANDS; i++) {
        const LearnedCommand& cmd = learnedDevice.commands[i];
        if (!cmd.valid) continue;
        
        // 编码Base64（先编码，再决定是否添加到JSON）
        String base64 = encodeTimingToBase64(cmd.timing, cmd.timingCount);
        if (base64.length() == 0) {
            // Base64编码失败（内存不足），跳过这个命令
            base64 = "";  // 确保释放
            continue;
        }
        
        // 只有Base64编码成功才添加到JSON
        if (!firstCommand) {
            json += ",";
        }
        firstCommand = false;
        validCommandCount++;
        
        json += "\"";
        // 确保cmd.name是有效的字符串
        if (cmd.name[0] != '\0') {
            json += cmd.name;
        }
        json += "\":\"";
        json += base64;
        base64 = "";  // 立即释放内存
        json += "\"";
    }
    
    // 如果没有有效的命令，返回空JSON
    if (validCommandCount == 0) {
        return "{}";
    }
    
    json += "}}";
    
    return json;
}

String IRLearner::encodeTimingToBase64(const IRTiming* timing, int count) {
    if (timing == nullptr || count == 0) {
        return "";
    }
    
    // 将时序数据转换为字节数组
    // 每个时序点：1字节（电平）+ 2字节（持续时间，单位10μs）
    // 为了兼容SmartIR，我们使用原始时序数据（微秒）
    // 限制最大时序点数以避免内存溢出
    int maxCount = (count > MAX_LEARN_TIMING_COUNT) ? MAX_LEARN_TIMING_COUNT : count;
    int byteCount = maxCount * 2;  // 每个时序点2字节（持续时间）
    
    // Base64编码后的字符串大小约为原始数据的4/3，加上一些开销
    size_t base64Size = (byteCount * 4 / 3) + 20;  // 增加一些余量
    
    // 检查可用内存，确保有足够空间（字节数组 + Base64字符串 + 大量缓冲）
    size_t requiredMemory = byteCount + base64Size + 3072;  // 增加缓冲到3KB
    if (ESP.getFreeHeap() < requiredMemory) {
        return "";  // 内存不足
    }
    
    // 检查最大连续块大小，确保可以分配连续内存
    size_t maxFreeBlock = ESP.getMaxFreeBlockSize();
    if (maxFreeBlock < byteCount + 512) {
        return "";  // 最大连续块不足
    }
    
    // 使用标准new，如果失败会抛出异常，但我们已经在前面检查了内存
    uint8_t* bytes = new uint8_t[byteCount];
    
    if (bytes == nullptr) {
        return "";  // 内存分配失败
    }
    
    // 将时序数据转换为字节数组（持续时间以10μs为单位）
    for (int i = 0; i < maxCount; i++) {
        uint16_t duration10us = (uint16_t)(timing[i].duration / 10);
        bytes[i * 2] = (uint8_t)(duration10us & 0xFF);
        bytes[i * 2 + 1] = (uint8_t)((duration10us >> 8) & 0xFF);
    }
    
    // Base64编码 - 使用reserve预分配内存
    String base64;
    if (!base64.reserve(base64Size)) {
        // reserve失败，释放内存并返回
        delete[] bytes;
        return "";
    }
    
    int i = 0;
    int j = 0;
    uint8_t char_array_3[3];
    uint8_t char_array_4[4];
    
    while (i < byteCount) {
        if (i >= byteCount) break;  // 安全检查
        char_array_3[j++] = bytes[i++];
        if (j == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            
            for (j = 0; j < 4; j++) {
                if (char_array_4[j] < 64) {  // 安全检查
                    base64 += base64_chars[char_array_4[j]];
                }
            }
            j = 0;
        }
    }
    
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
    
    delete[] bytes;
    return base64;
}

