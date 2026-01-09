#include "ir_learner.h"
#include <ArduinoJson.h>

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
    // 学习完成后，mode 变为 LEARN_NONE，但 learnedDevice 仍然有效
    // 所以需要检查 learnedDevice.valid 而不是 mode
    if (!learnedDevice.valid) {
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
    
    // 验证信号长度
    // 最小有效信号长度：至少需要10个时序点（IRParser要求）
    const int MIN_VALID_TIMING_COUNT = 10;
    if (count < MIN_VALID_TIMING_COUNT) {
        Serial.println("错误：信号太短，无法学习");
        return;
    }
    
    // 检查信号是否超过最大长度限制
    bool signalTruncated = false;
    int actualTimingCount = count;
    if (count > MAX_LEARN_TIMING_COUNT) {
        actualTimingCount = MAX_LEARN_TIMING_COUNT;
        signalTruncated = true;
        // 记录警告：信号被截断
        Serial.printf("警告：信号长度 %d 超过最大限制 %d，将被截断\n", count, MAX_LEARN_TIMING_COUNT);
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
    cmd.timingCount = actualTimingCount;
    
    // 确保截断后的信号仍然有效
    // 如果信号被截断，验证截断后的信号是否仍然有效
    if (signalTruncated) {
        // 使用截断后的信号重新解析，确保仍然有效
        SignalID truncatedSignalId;
        if (!parser.parseSignal(timing, actualTimingCount, truncatedSignalId)) {
            Serial.println("错误：截断后的信号无效，无法学习");
            return;
        }
        // 使用截断后的信号ID（可能不同，但仍然是有效的信号）
        cmd.signalId = truncatedSignalId;
    }
    
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
        Serial.println("[convertToSmartIR] 错误：设备无效或命令数为0");
        return "{}";
    }
    
    // 首先检查内存，确保有足够空间
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 6144) {  // 至少需要6KB空闲内存
        Serial.printf("[convertToSmartIR] 错误：内存严重不足（需要: 6144, 可用: %d）\n", freeHeap);
        return "{}";  // 内存不足，返回空JSON
    }
    
    // 更准确地估算所需内存，考虑最坏情况：
    // 1. 使用每个命令的实际时序点数（最坏情况：200个时序点 = 400字节 = 约533字节Base64）
    // 2. JSON中字符串需要转义和引号
    // 3. ArduinoJson需要额外的开销来存储字符串引用
    
    // 估算每个命令的实际大小，使用实际时序点数
    size_t totalBase64Size = 0;
    int maxTimingCount = 0;  // 记录最大时序点数，用于动态调整系数
    
    for (int i = 0; i < learnedDevice.commandCount && i < MAX_LEARN_COMMANDS; i++) {
        const LearnedCommand& cmd = learnedDevice.commands[i];
        if (!cmd.valid) continue;
        
        // 记录最大时序点数
        if (cmd.timingCount > maxTimingCount) {
            maxTimingCount = cmd.timingCount;
        }
        
        // 每个时序点2字节，Base64编码后约2.67字节（4/3）
        int byteCount = cmd.timingCount * 2;
        size_t base64Size = (byteCount * 4 / 3) + 4;  // 加上填充
        // JSON中字符串需要转义和引号，实际需要更多空间
        // 命令名称 + 冒号 + 引号 + Base64字符串 + 引号 + 逗号
        size_t cmdSize = strlen(cmd.name) + base64Size + 20;  // 加上JSON结构开销
        totalBase64Size += cmdSize;
    }
    
    // 基础开销：设备名称 + JSON结构
    size_t baseOverhead = 100 + strlen(learnedDevice.deviceName) + 50;
    
    // ArduinoJson需要额外的内存来管理字符串和对象
    // 根据信号长度动态调整系数：
    // - 短信号（<100个时序点）：1.3倍
    // - 中等信号（100-150个时序点）：1.5倍
    // - 长信号（>150个时序点）：1.8倍（长信号需要更多管理开销）
    float multiplier = 1.5f;  // 默认1.5倍
    if (maxTimingCount > 150) {
        multiplier = 1.8f;  // 长信号使用更大的系数
    } else if (maxTimingCount < 100) {
        multiplier = 1.3f;  // 短信号使用更小的系数
    }
    
    size_t estimatedJsonSize = baseOverhead + (size_t)(totalBase64Size * multiplier);
    
    // 限制最大大小，避免内存不足
    if (estimatedJsonSize > 16384) {  // 限制最大16KB
        estimatedJsonSize = 16384;
    }
    
    // 检查内存是否足够（降低安全余量）
    // 只要可用内存大于估算值，就尝试分配（ArduinoJson会动态调整）
    if (freeHeap < estimatedJsonSize + 1024) {  // 安全余量1KB
        // 不直接返回，继续尝试转换，ArduinoJson会动态分配内存
    }
    
    // 使用ArduinoJson构建JSON，避免手动字符串拼接
    DynamicJsonDocument doc(estimatedJsonSize);
    
    // 检查文档是否有效（分配成功）
    size_t actualCapacity = doc.capacity();
    if (actualCapacity == 0) {
        Serial.println("[convertToSmartIR] 错误：内存分配失败");
        return "{}";  // 内存分配失败
    }
    
    // 设置设备名称
    if (learnedDevice.deviceName[0] != '\0') {
        doc["deviceName"] = learnedDevice.deviceName;
    } else {
        doc["deviceName"] = "";
    }
    
    // 创建commands对象
    JsonObject commands = doc.createNestedObject("commands");
    if (commands.isNull()) {
        return "{}";  // 创建对象失败
    }
    
    int validCommandCount = 0;
    
    // 遍历所有命令，逐个编码并添加到JSON
    for (int i = 0; i < learnedDevice.commandCount && i < MAX_LEARN_COMMANDS; i++) {
        const LearnedCommand& cmd = learnedDevice.commands[i];
        if (!cmd.valid) {
            continue;
        }
        
        // 编码Base64
        String base64 = encodeTimingToBase64(cmd.timing, cmd.timingCount);
        if (base64.length() == 0) {
            // Base64编码失败（内存不足），跳过这个命令
            Serial.printf("[convertToSmartIR] 警告：命令 %d Base64编码失败，跳过\n", i);
            continue;
        }
        
        // 检查JSON文档容量是否足够添加这个命令
        // ArduinoJson在添加String对象时会复制字符串，需要足够的容量
        size_t currentUsage = doc.memoryUsage();
        size_t availableCapacity = doc.capacity() - currentUsage;
        
        // 根据Base64长度动态调整容量检查阈值
        // 短信号（<200字节）：需要Base64长度 + 30字节
        // 中等信号（200-400字节）：需要Base64长度 + 50字节
        // 长信号（>400字节）：需要Base64长度 + 80字节（长信号需要更多管理开销）
        size_t requiredMargin = 30;  // 默认30字节
        if (base64.length() > 400) {
            requiredMargin = 80;  // 长信号需要更多余量
        } else if (base64.length() > 200) {
            requiredMargin = 50;  // 中等信号需要中等余量
        }
        
        // 放宽容量检查：只要可用容量大于Base64字符串长度加上动态余量，就尝试添加
        // ArduinoJson会动态管理内存，即使接近容量限制也能正常工作
        if (availableCapacity < base64.length() + requiredMargin) {
            Serial.printf("[convertToSmartIR] 警告：命令 %d 容量不足，跳过（可用: %d, Base64长度: %d, 需要余量: %d）\n", 
                         i, availableCapacity, base64.length(), requiredMargin);
            base64 = "";  // 释放内存
            continue;
        }
        
        // 添加到JSON对象
        const char* cmdName = (cmd.name[0] != '\0') ? cmd.name : "";
        size_t usageBefore = doc.memoryUsage();
        
        // 添加字符串到JSON（ArduinoJson会复制字符串）
        commands[cmdName] = base64;
        
        // 检查是否添加成功
        size_t usageAfter = doc.memoryUsage();
        if (usageAfter <= usageBefore) {
            // 添加失败，可能容量不足，跳过这个命令
            Serial.printf("[convertToSmartIR] 警告：命令 %d 添加失败，跳过\n", i);
            base64 = "";  // 释放内存
            continue;
        }
        
        base64 = "";  // 立即释放内存
        validCommandCount++;
        
        // 放宽容量检查：只要还有可用容量，就继续添加命令
        if (usageAfter >= doc.capacity() * 0.99) {
            // 容量使用率超过99%，停止添加更多命令
            break;
        }
        
        // 检查内存是否仍然足够继续处理
        size_t freeHeapNow = ESP.getFreeHeap();
        if (freeHeapNow < 3072) {  // 降低阈值到3KB
            Serial.printf("[convertToSmartIR] 警告：内存不足，停止处理剩余命令（可用: %d）\n", freeHeapNow);
            break;
        }
    }
    
    // 如果没有有效的命令，返回空JSON
    if (validCommandCount == 0) {
        return "{}";
    }
    
    // 序列化为字符串
    // 使用 measureJson 来准确测量序列化后的大小
    size_t serializedSize = measureJson(doc);
    if (serializedSize == 0) {
        Serial.println("[convertToSmartIR] 错误：测量失败");
        return "{}";  // 测量失败
    }
    
    // 加上一些余量（JSON转义可能需要更多空间）
    serializedSize += 256;
    
    // 限制最大大小
    if (serializedSize > 16384) {
        serializedSize = 16384;  // 限制最大16KB
    }
    
    // 检查内存是否足够序列化
    size_t freeHeapBeforeSerialize = ESP.getFreeHeap();
    if (freeHeapBeforeSerialize < serializedSize + 1024) {  // 安全余量1KB
        // 不直接返回，继续尝试序列化
    }
    
    String output;
    // 尝试预分配，但不要强制
    if (!output.reserve(serializedSize)) {
        output.reserve(serializedSize / 2);
    }
    
    // 序列化JSON
    size_t bytesWritten = serializeJson(doc, output);
    
    // 检查序列化是否成功
    if (bytesWritten == 0 || output.length() == 0) {
        Serial.println("[convertToSmartIR] 错误：序列化失败");
        return "{}";  // 序列化失败
    }
    
    return output;
}

String IRLearner::encodeTimingToBase64(const IRTiming* timing, int count) {
    if (timing == nullptr || count == 0) {
        return "";
    }
    
    // 限制最大时序点数以避免内存溢出
    int maxCount = (count > MAX_LEARN_TIMING_COUNT) ? MAX_LEARN_TIMING_COUNT : count;
    int byteCount = maxCount * 2;  // 每个时序点2字节（持续时间）
    
    // Base64编码后的字符串大小约为原始数据的4/3，加上一些开销
    size_t base64Size = (byteCount * 4 / 3) + 20;  // 增加一些余量
    
    // 检查可用内存，确保有足够空间（Base64字符串 + 缓冲）
    // 对于长信号，需要更多缓冲空间
    size_t bufferMargin = (maxCount > 150) ? 3072 : 2048;  // 长信号需要3KB缓冲
    size_t requiredMemory = base64Size + bufferMargin;
    if (ESP.getFreeHeap() < requiredMemory) {
        return "";  // 内存不足
    }
    
    // 根据信号长度动态调整栈缓冲区大小
    // 短信号（<=100个时序点）：使用栈缓冲区（200字节）
    // 中等信号（101-200个时序点）：使用栈缓冲区（400字节）
    // 超长信号（>200个时序点）：使用动态分配
    uint8_t* bytes = nullptr;
    bool useStackBuffer = false;
    uint8_t stackBuffer[400];  // 最大400字节栈缓冲区
    
    if (byteCount <= 200) {
        // 短信号：使用200字节栈缓冲区
        bytes = stackBuffer;
        useStackBuffer = true;
    } else if (byteCount <= 400) {
        // 中等信号：使用400字节栈缓冲区
        bytes = stackBuffer;
        useStackBuffer = true;
    } else {
        // 超长信号：使用动态分配
        // 检查最大连续块大小
        size_t maxFreeBlock = ESP.getMaxFreeBlockSize();
        if (maxFreeBlock < byteCount + 512) {
            return "";  // 最大连续块不足
        }
        
        // 使用动态分配
        bytes = new uint8_t[byteCount];
        if (bytes == nullptr) {
            return "";  // 内存分配失败
        }
        useStackBuffer = false;
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
        if (!useStackBuffer && bytes != nullptr) {
            delete[] bytes;
        }
        return "";
    }
    
    // Base64编码循环
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
    
    // 释放动态分配的内存
    if (!useStackBuffer && bytes != nullptr) {
        delete[] bytes;
    }
    
    return base64;
}

