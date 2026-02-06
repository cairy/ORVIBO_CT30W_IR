#include "api_handlers.h"
#include "ir_core/unified_signal.h"
#include "config/constants.h"
#include <LittleFS.h>
#include <new>

String APIHandlers::handleQueryMaps(IRMapper& mapper, Logger* logger) {
    // 优化：直接读取文件内容，避免重新构建JSON文档（节省大量内存）
    // 文件已经是以hex格式保存的，格式正确，可以直接返回
    if (LittleFS.exists(CONFIG_FILE_MAPS)) {
        File file = LittleFS.open(CONFIG_FILE_MAPS, "r");
        if (file) {
            size_t fileSize = file.size();
            // 检查文件大小是否合理
            if (fileSize > 0 && fileSize < 32768) {  // 最大32KB
                String content = file.readString();
                file.close();
                
                // 验证内容是否为有效的JSON数组
                String trimmed = content;
                trimmed.trim();
                if (trimmed.length() > 0 && trimmed[0] == '[') {
                    // 检查消息大小，如果接近或超过MQTT缓冲区限制，记录警告
                    size_t estimatedMqttSize = content.length() + 256;  // 估算包装开销
                    if (logger != nullptr && estimatedMqttSize > MQTT_BUFFER_SIZE * 0.8) {
                        char warnMsg[256];
                        snprintf(warnMsg, sizeof(warnMsg), 
                                 "[API: /api/maps] 文件大小: %d bytes, 估算MQTT消息大小: %d bytes (缓冲区: %d bytes, %.1f%%)", 
                                 content.length(), estimatedMqttSize, MQTT_BUFFER_SIZE, 
                                 (estimatedMqttSize * 100.0f / MQTT_BUFFER_SIZE));
                        logger->log(estimatedMqttSize > MQTT_BUFFER_SIZE ? "error" : "warning", warnMsg);
                    }
                    return content;
                }
            }
            file.close();
        }
    }
    
    // 如果文件不存在或读取失败，使用传统方法重新构建
    int count = mapper.getRuleCount();
    if (count == 0) {
        // 如果没有规则，返回空数组
        return "[]";
    }
    
    size_t jsonSize = MemoryMonitor::calculateRulesJsonSize(count);
    if (!MemoryMonitor::checkMemory(jsonSize + 1024)) {
        return "";  // 内存不足，返回空字符串
    }
    
    // 使用堆分配，避免栈溢出
    IR_MapRule* rules = new (std::nothrow) IR_MapRule[count];
    if (rules == nullptr) {
        return "";  // 分配失败
    }
    
    // 获取所有规则，返回实际填充的数量
    int actualCount = mapper.getAllRules(rules, count);
    if (actualCount == 0) {
        delete[] rules;
        return "[]";
    }
    
    // 构建JSON（只返回data部分）
    // 使用保守估算：每条规则约2000字节
    const size_t MAX_DOC_SIZE = 16384;  // 最大文档大小16KB
    size_t totalSize = 512 + (actualCount * 2000);
    if (totalSize < 4096) totalSize = 4096;
    if (totalSize > MAX_DOC_SIZE) totalSize = MAX_DOC_SIZE;
    
    DynamicJsonDocument doc(totalSize);
    JsonArray rulesArray = doc.to<JsonArray>();
    
    // 使用统一的转换方法
    for (int i = 0; i < actualCount; i++) {
        JsonObject ruleObj = rulesArray.createNestedObject();
        IRMapper::ruleToJsonObject(rules[i], ruleObj, true);  // true表示使用hex格式
    }
    
    // 检查文档是否溢出
    if (doc.overflowed()) {
        delete[] rules;
        if (logger != nullptr) {
            logger->log("error", "[API: /api/maps] JSON文档溢出，无法构建规则");
        }
        return "";
    }
    
    // 序列化
    String output;
    serializeJson(doc, output);
    delete[] rules;

    if (doc.overflowed()) {
        if (logger != nullptr) {
            logger->log("error", "[API: /api/maps] JSON文档溢出，无法序列化规则");
        }
        return "";
    }
    
    return output;
}

String APIHandlers::handleUpdateMap(IRMapper& mapper, const String& json, bool& success) {
    success = false;
    
    size_t jsonSize = json.length() + 1024;
    if (jsonSize > 6144) jsonSize = 6144;
    if (jsonSize < 512) jsonSize = 512;
    
    if (!MemoryMonitor::checkMemory(jsonSize + 2048)) {
        return "Low Memory";
    }
    
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, json);
    
    if (error) {
        return "Invalid JSON";
    }
    
    // 检查是否是批量更新（包含rules数组）
    if (doc.containsKey("rules")) {
        // 批量更新：清空现有规则，添加新规则（堆分配避免栈溢出）
        JsonArray rulesArray = doc["rules"];
        
        int count = mapper.getRuleCount();
        if (count > 0) {
            IR_MapRule* rules = new (std::nothrow) IR_MapRule[count];
            if (rules != nullptr) {
                int actualCount = mapper.getAllRules(rules, count);
                for (int i = 0; i < actualCount; i++) {
                    mapper.deleteRule(rules[i].id);
                }
                delete[] rules;
            }
        }
        
        // 添加新规则（使用统一的转换方法）
        int successCount = 0;
        for (JsonObject ruleObj : rulesArray) {
            IR_MapRule rule;
            if (IRMapper::jsonObjectToRule(ruleObj, rule)) {
                if (mapper.addRule(rule)) {
                    successCount++;
                }
            }
        }
        
        char responseMsg[64];
        snprintf(responseMsg, sizeof(responseMsg), "Rules updated, count: %d", successCount);
        success = true;
        return String(responseMsg);
    } else {
        // 单个更新（保持向后兼容）
        uint8_t id = doc["id"] | 0;
        if (id == 0) {
            return "Invalid rule ID or missing 'rules' array";
        }
        
        JsonObject ruleObj = doc["rule"];
        if (ruleObj.isNull()) {
            return "Missing 'rule' field";
        }
        
        IR_MapRule rule;
        if (IRMapper::jsonObjectToRule(ruleObj, rule)) {
            if (mapper.updateRule(id, rule)) {
                success = true;
                return "Rule updated";
            } else {
                return "Failed to update rule";
            }
        } else {
            return "Invalid rule format";
        }
    }
}

String APIHandlers::handleGetLogs(Logger& logger) {
    int count = logger.getLogCount();
    size_t jsonSize = MemoryMonitor::calculateLogsJsonSize(count);
    
    if (!MemoryMonitor::checkMemory(jsonSize + 1024)) {
        return "";  // 内存不足
    }
    
    LogEntry entries[MAX_LOG_ENTRIES];
    logger.getLogs(entries, count);
    
    // 构建JSON（只返回data部分）
    size_t totalSize = jsonSize + 256;
    DynamicJsonDocument doc(totalSize);
    JsonArray logArray = doc.to<JsonArray>();
    
    for (int i = 0; i < count; i++) {
        JsonObject entry = logArray.createNestedObject();
        entry["timestamp"] = entries[i].timestamp;
        entry["type"] = entries[i].type;
        entry["content"] = entries[i].content;
    }
    
    String output;
    serializeJson(doc, output);
    return output;
}

String APIHandlers::handleStartLearn(IRLearner& learner, const String& json, bool& success) {
    success = false;
    
    if (!MemoryMonitor::checkMemory(2048)) {
        return "Low Memory";
    }
    
    // 解析JSON
    size_t jsonSize = json.length() + 256;
    if (jsonSize > 1024) jsonSize = 1024;
    if (jsonSize < 256) jsonSize = 256;
    
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, json);
    
    if (error) {
        return "Invalid JSON";
    }
    
    String deviceName = doc["deviceName"] | "";
    String commandNames = doc["commandNames"] | "";
    int timeoutSeconds = doc["timeoutSeconds"] | DEFAULT_LEARN_TIMEOUT_SEC;
    
    if (deviceName.length() == 0) {
        return "Missing 'deviceName' field";
    }
    
    if (commandNames.length() == 0) {
        return "Missing 'commandNames' field";
    }
    
    // 先检查命令数量是否超过限制
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
    
    // 检查命令数量限制
    if (commandCount > MAX_LEARN_COMMANDS) {
        char errorMsg[128];
        snprintf(errorMsg, sizeof(errorMsg), "命令数量超过限制，最多支持 %d 个命令，当前有 %d 个", MAX_LEARN_COMMANDS, commandCount);
        return String(errorMsg);
    }
    
    // 启动学习
    if (learner.startDeviceLearn(deviceName, commandNames, timeoutSeconds)) {
        success = true;
        return "学习已启动";
    } else {
        return "Failed to start learning (可能已有学习任务进行中)";
    }
}

String APIHandlers::handleQueryLearnStatus(IRLearner& learner) {
    if (!MemoryMonitor::checkMemory(512)) {
        return "";  // 内存不足
    }
    
    bool isLearning = learner.isLearningDevice();
    
    // 使用简化版本（只返回isLearning, progress, currentCommand）
    DynamicJsonDocument doc(512);
    JsonObject dataObj = doc.to<JsonObject>();
    dataObj["isLearning"] = isLearning;
    
    if (isLearning) {
        String progress = learner.getLearnProgress();
        dataObj["progress"] = progress;
        
        String currentCommand = learner.getCurrentCommandName();
        dataObj["currentCommand"] = currentCommand;
    } else {
        dataObj["progress"] = "";
        dataObj["currentCommand"] = "";
    }
    
    String output;
    serializeJson(doc, output);
    return output;
}

String APIHandlers::handleStopLearn(IRLearner& learner, bool& success) {
    success = false;
    
    if (learner.stopDeviceLearn()) {
        success = true;
        return "Device learn stopped";
    } else {
        return "Device learn is not active";
    }
}

String APIHandlers::handleQueryLearnResult(IRLearner& learner, const String& format) {
    int commandCount = learner.getLearnedCommandCount();
    if (commandCount == 0) {
        return "";  // 没有学习结果
    }
    
    // 内存检查：转换结果可能很大（最多3KB），需要更多内存
    if (!MemoryMonitor::checkMemory(4096)) {
        return "";  // 内存不足
    }
    
    String formatLower = format;
    formatLower.toLowerCase();
    
    String result;
    // 默认返回字符串格式
    if (formatLower.length() == 0 || formatLower == "string") {
        result = learner.convertToStringFormat();
    } else if (formatLower == "json") {
        result = learner.convertToJSONFormat();
    } else {
        // 默认字符串格式
        result = learner.convertToStringFormat();
    }
    
    if (result.length() == 0) {
        return "";  // 转换失败
    }
    
    // 所有格式都返回JSON（包括字符串格式）
    // JSON格式：解析为JSON对象，然后返回（只返回data部分）
    size_t resultSize = result.length() + 256;
    DynamicJsonDocument resultDoc(resultSize);
    DeserializationError err = deserializeJson(resultDoc, result);
    
    if (err) {
        // 如果不是JSON，直接返回字符串（向后兼容）
        return result;
    } else {
        // 如果是JSON，返回序列化后的JSON字符串
        String output;
        serializeJson(resultDoc, output);
        return output;
    }
}

String APIHandlers::handleGetRecentSignals(Logger& logger) {
    if (!MemoryMonitor::checkMemory(2048)) {
        return "";  // 内存不足
    }
    
    SignalSequence sequences[MAX_RECENT_SIGNALS];
    int count = logger.getRecentSignalSequences(sequences, MAX_RECENT_SIGNALS);
    
    // 构建JSON（只返回data部分）
    // 估算JSON大小：基础256字节 + 每个序列约200-500字节（取决于信号数量）
    size_t jsonSize = 256 + (count * 500);
    if (jsonSize > 2048) jsonSize = 2048;
    if (jsonSize < 512) jsonSize = 512;
    
    DynamicJsonDocument doc(jsonSize);
    JsonArray sequencesArray = doc.to<JsonArray>();
    
    for (int i = 0; i < count; i++) {
        JsonObject seqObj = sequencesArray.createNestedObject();
        SignalUtils::sequenceToJSON(sequences[i], seqObj, true);
    }
    
    String output;
    serializeJson(doc, output);
    return output;
}

String APIHandlers::handleSendSignal(IRForwarder& forwarder, const String& payload, bool& success) {
    success = false;
    
    if (!MemoryMonitor::checkMemory(2048)) {
        return "Low Memory";
    }
    
    String trimmed = payload;
    trimmed.trim();
    
    if (trimmed.length() == 0) {
        return "Empty payload";
    }
    
    // 统一使用parseAndSendSignal，支持JSON和字符串两种格式
    if (forwarder.parseAndSendSignal(trimmed)) {
        success = true;
        return "Signal sent";
    } else {
        return "Failed to send signal";
    }
}
