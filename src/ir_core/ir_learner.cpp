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
        if (!receiver.isReceiving()) {
            static IRTiming timing[MAX_LEARN_TIMING_COUNT];
            int count = 0;
            
            if (receiver.getSignal(timing, count, MAX_LEARN_TIMING_COUNT)) {
                lastSignalTime = millis();
                processDeviceLearnSignal();
            }
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
        return false;  // 没有有效的指令名称
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
    
    return String(currentCommandIndex + 1) + "/" + String(commandNamesCount);
}

bool IRLearner::getLearnedDevice(LearnedDevice& device) {
    if (!learnedDevice.valid) {
        return false;
    }
    
    device = learnedDevice;
    return true;
}

void IRLearner::processDeviceLearnSignal() {
    if (currentCommandIndex >= commandNamesCount || currentCommandIndex >= MAX_LEARN_COMMANDS) {
        return;  // 所有指令已学习完成
    }
    
    static IRTiming timing[MAX_LEARN_TIMING_COUNT];
    int count = 0;
    
    if (!receiver.getSignal(timing, count, MAX_LEARN_TIMING_COUNT)) {
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
    
    String json = "{";
    json += "\"deviceName\":\"" + String(learnedDevice.deviceName) + "\",";
    json += "\"commands\":{";
    
    for (int i = 0; i < learnedDevice.commandCount; i++) {
        const LearnedCommand& cmd = learnedDevice.commands[i];
        if (!cmd.valid) continue;
        
        if (i > 0) json += ",";
        json += "\"" + String(cmd.name) + "\":\"";
        json += encodeTimingToBase64(cmd.timing, cmd.timingCount);
        json += "\"";
    }
    
    json += "}";
    json += "}";
    
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
    
    // 检查可用内存，确保有足够空间
    if (ESP.getFreeHeap() < byteCount + 1024) {  // 额外保留1KB缓冲
        return "";  // 内存不足
    }
    
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
    
    // Base64编码
    String base64 = "";
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

