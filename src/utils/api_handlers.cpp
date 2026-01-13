#include "api_handlers.h"
#include "ir_core/unified_signal.h"

String APIHandlers::handleQueryMaps(IRMapper& mapper, Logger* logger) {
    int count = mapper.getRuleCount();
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
        // 如果没有规则，返回空数组
        DynamicJsonDocument doc(256);
        doc.to<JsonArray>();  // 创建空数组
        String output;
        serializeJson(doc, output);
        return output;
    }
    
    // 构建JSON（只返回data部分）
    // 增加更大的缓冲：JSON文档内存需求通常是估算大小的1.5-2倍
    // 特别是使用hex格式时，字符串会更长
    size_t totalSize = (jsonSize * 2) + 1024;  // 2倍 + 1KB缓冲
    if (totalSize < 2048) totalSize = 2048;  // 最小2KB
    if (totalSize > 16384) totalSize = 16384;  // 限制最大16KB
    DynamicJsonDocument doc(totalSize);
    JsonArray rulesArray = doc.to<JsonArray>();
    
    // 使用统一的转换方法，使用实际填充的数量
    for (int i = 0; i < actualCount; i++) {
        JsonObject ruleObj = rulesArray.createNestedObject();
        IRMapper::ruleToJsonObject(rules[i], ruleObj, true);  // true表示使用hex格式
    }
    
    // 检查文档是否溢出
    if (doc.overflowed()) {
        char buffer[192];
        snprintf(buffer, sizeof(buffer), "[API: /api/maps] JSON文档溢出，尝试使用更大的文档 (当前大小: %d bytes, 规则数: %d)", totalSize, actualCount);
        if (logger != nullptr) {
            logger->log("warning", buffer);
        }
        
        // 尝试使用更大的文档重新构建
        size_t largerSize = totalSize * 2;
        if (largerSize <= 16384) {  // 限制最大16KB
            DynamicJsonDocument largerDoc(largerSize);
            JsonArray largerArray = largerDoc.to<JsonArray>();
            for (int i = 0; i < actualCount; i++) {
                JsonObject ruleObj = largerArray.createNestedObject();
                IRMapper::ruleToJsonObject(rules[i], ruleObj, true);
            }
            if (!largerDoc.overflowed()) {
                doc = largerDoc;
            } else {
                char errorBuffer[192];
                snprintf(errorBuffer, sizeof(errorBuffer), "[API: /api/maps] 更大的文档仍然溢出 (大小: %d bytes, 规则数: %d)", largerSize, actualCount);
                if (logger != nullptr) {
                    logger->log("error", errorBuffer);
                }
            }
        } else {
            char errorBuffer[192];
            snprintf(errorBuffer, sizeof(errorBuffer), "[API: /api/maps] JSON文档过大，无法重试 (需要: %d bytes, 规则数: %d)", largerSize, actualCount);
            if (logger != nullptr) {
                logger->log("error", errorBuffer);
            }
        }
    }
    
    // 立即释放规则数组内存
    delete[] rules;
    
    String output;
    serializeJson(doc, output);
    
    // 如果文档溢出，返回空字符串（表示失败）
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
        // 批量更新：清空现有规则，添加新规则
        JsonArray rulesArray = doc["rules"];
        
        // 清空所有现有规则
        IR_MapRule rules[MAX_MAP_RULES];
        int count = mapper.getRuleCount();
        int actualCount = mapper.getAllRules(rules, count);
        for (int i = 0; i < actualCount; i++) {
            mapper.deleteRule(rules[i].id);
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
    
    String resultJson;
    if (formatLower == "signalid") {
        resultJson = learner.convertToSignalIDFormat();
    } else if (formatLower == "array") {
        resultJson = learner.convertToArrayFormat();
    } else {
        // 默认使用base64格式
        resultJson = learner.convertToBase64Format();
    }
    
    if (resultJson.length() == 0 || resultJson == "{}") {
        return "";  // 转换失败
    }
    
    // 解析resultJson为JSON对象，然后返回（只返回data部分）
    size_t resultSize = resultJson.length() + 256;
    DynamicJsonDocument resultDoc(resultSize);
    DeserializationError err = deserializeJson(resultDoc, resultJson);
    
    if (err) {
        // 如果不是JSON，直接返回字符串
        return resultJson;
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
    
    RecentSignal signals[MAX_RECENT_SIGNALS];
    int count = logger.getRecentSignals(signals, MAX_RECENT_SIGNALS);
    
    // 构建JSON（只返回data部分）
    size_t jsonSize = 256 + (count * 100);
    if (jsonSize > 1536) jsonSize = 1536;
    if (jsonSize < 512) jsonSize = 512;
    
    DynamicJsonDocument doc(jsonSize);
    JsonArray signalsArray = doc.to<JsonArray>();
    
    for (int i = 0; i < count; i++) {
        JsonObject signalObj = signalsArray.createNestedObject();
        signalObj["timestamp"] = signals[i].timestamp;
        signalObj["matched"] = signals[i].matched;
        
        JsonObject signal = signalObj.createNestedObject("signal");
        SignalUtils::signalToJSON(signals[i].signal, signal, true);
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
    
    // 检测格式
    IRLearner::SignalFormat format = IRLearner::detectSignalFormat(trimmed);
    
    // 如果检测到JSON格式（FORMAT_SIGNALID），按JSON格式解析
    if (format == IRLearner::FORMAT_SIGNALID) {
        // UnifiedSignal对象格式（JSON）
        size_t jsonSize = trimmed.length() + 256;
        if (jsonSize > 1024) jsonSize = 1024;
        if (jsonSize < 256) jsonSize = 256;
        
        if (!MemoryMonitor::checkMemory(jsonSize + 512)) {
            return "Low Memory";
        }
        
        DynamicJsonDocument doc(jsonSize);
        DeserializationError error = deserializeJson(doc, trimmed);
        
        if (error) {
            return "Invalid JSON";
        }
        
        JsonObject signalObj = doc["signal"];
        if (signalObj.isNull()) {
            return "Missing 'signal' field";
        }
        
        uint8_t repeatCount = doc["repeatCount"] | 1;
        
        UnifiedSignal signal;
        if (!SignalUtils::signalFromJSON(signalObj, signal)) {
            return "Invalid signal format";
        }
        signal.repeatCount = repeatCount;
        
        if (forwarder.sendSignal(signal, repeatCount)) {
            success = true;
            return "Signal sent";
        } else {
            return "Failed to send signal";
        }
    } else {
        // 使用统一的解析和发送方法（字符串格式）
        if (forwarder.parseAndSendSignal(trimmed)) {
            success = true;
            return "Signal sent";
        } else {
            return "Failed to send signal";
        }
    }
}
