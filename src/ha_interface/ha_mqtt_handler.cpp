#include "ha_mqtt_handler.h"
#include "ir_core/ir_learner.h"
#include <ArduinoJson.h>

HAMQTTHandler* HAMQTTHandler::instance = nullptr;

HAMQTTHandler::HAMQTTHandler(MQTTClient& mqtt, IRMapper& mapper,
                             IRForwarder& forwarder, Logger& logger)
    : mqtt(mqtt), mapper(mapper), forwarder(forwarder), logger(logger), 
      learner(nullptr), discoveryPublished(false), 
      lastDiscoveryAttempt(0), firstConnectedTime(0), discoveryRetryCount(0),
      lastStatePublishTime(0) {
    instance = this;
}

bool HAMQTTHandler::init() {
    mqtt.setCallback(mqttCallback);
    // 订阅将在 MQTT 连接成功后进行
    return true;
}

void HAMQTTHandler::update() {
    mqtt.update();
    
    // 当 MQTT 连接成功时
    static bool wasConnected = false;
    bool isConnected = mqtt.isConnected();
    
    if (isConnected && !wasConnected) {
        // 刚连接成功，订阅命令主题
        // 检查内存后再操作
        if (ESP.getFreeHeap() >= 1024) {
            Serial.println("[HA MQTT] MQTT 已连接，订阅命令主题...");
            if (mqtt.subscribe(MQTT_TOPIC_COMMAND)) {
                Serial.println("[HA MQTT] ✓ 已订阅: " + String(MQTT_TOPIC_COMMAND));
            } else {
                Serial.println("[HA MQTT] ✗ 订阅失败: " + String(MQTT_TOPIC_COMMAND));
            }
        } else {
            Serial.printf("[HA MQTT] 内存不足，跳过订阅 (空闲: %d bytes)\n", ESP.getFreeHeap());
        }
        wasConnected = true;
    } else if (!isConnected && wasConnected) {
        // 连接断开
        Serial.println("[HA MQTT] MQTT 连接已断开");
        wasConnected = false;
        discoveryPublished = false;
        lastDiscoveryAttempt = 0;
        firstConnectedTime = 0;
        discoveryRetryCount = 0;
    }
    
    // 当 MQTT 连接成功且未发布 Discovery 时，发布配置
    // 使用递增重试间隔：5秒、10秒、20秒，避免频繁重试占用资源
    if (isConnected && !discoveryPublished) {
        unsigned long now = millis();
        
        // 确保连接稳定后再发布（连接后至少等待 1 秒）
        if (firstConnectedTime == 0) {
            firstConnectedTime = now;
            discoveryRetryCount = 0;
        }
        
        if (now - firstConnectedTime >= 1000) {
            // 计算重试间隔：第1次5秒，第2次10秒，第3次及以后20秒
            unsigned long retryInterval = 5000;  // 默认5秒
            if (discoveryRetryCount == 1) {
                retryInterval = 10000;  // 第2次10秒
            } else if (discoveryRetryCount >= 2) {
                retryInterval = 20000;  // 第3次及以后20秒
            }
            
            if (now - lastDiscoveryAttempt >= retryInterval) {
                lastDiscoveryAttempt = now;
                discoveryRetryCount++;
                publishDiscoveryConfig();
            }
        }
    } else if (!isConnected) {
        // 连接断开时，重置重试计数
        firstConnectedTime = 0;
        discoveryRetryCount = 0;
    }
    
    // 定期发布增强的状态信息（每5秒）
    if (isConnected && discoveryPublished) {
        unsigned long now = millis();
        if (now - lastStatePublishTime >= STATE_PUBLISH_INTERVAL) {
            lastStatePublishTime = now;
            publishEnhancedState();
        }
    }
}

void HAMQTTHandler::handleCommand(const String& topic, const String& payload) {
    if (topic == MQTT_TOPIC_COMMAND) {
        processCommand(payload);
    }
}

void HAMQTTHandler::publishState(const String& state, const String& message) {
    // 使用增强的状态发布
    publishEnhancedState();
}

void HAMQTTHandler::publishLog(const String& logEntry) {
    mqtt.publish(MQTT_TOPIC_LOG, logEntry);
}

void HAMQTTHandler::publishDiscoveryConfig() {
    if (discoveryPublished) return;
    
    if (!mqtt.isConnected()) {
        Serial.println("[HA Discovery] MQTT 未连接，无法发布 Discovery 配置");
        return;
    }
    
    // 检查可用内存，如果内存不足则跳过发布
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 4096) {  // 至少需要4KB空闲内存
        Serial.printf("[HA Discovery] 内存不足，跳过发布 (空闲: %d bytes)\n", freeHeap);
        return;
    }
    
    Serial.println("[HA Discovery] 开始发布 Home Assistant Discovery 配置...");
    Serial.printf("[HA Discovery] MQTT 缓冲区大小: %d bytes, 空闲内存: %d bytes\n", 
                  MQTT_BUFFER_SIZE, freeHeap);
    
    // 使用const char*避免不必要的String对象创建
    const char* deviceId = MQTT_DEVICE_ID;
    const char* deviceName = MQTT_DEVICE_NAME;
    char chipIdStr[16];
    snprintf(chipIdStr, sizeof(chipIdStr), "%u", ESP.getChipId());
    
    bool allPublished = true;
    
    // 设备信息（使用较小的缓冲区）
    DynamicJsonDocument deviceDoc(384);
    JsonArray identifiers = deviceDoc["identifiers"].to<JsonArray>();
    char identifier[64];
    snprintf(identifier, sizeof(identifier), "orvibo_ct30w_%s", chipIdStr);
    identifiers.add(identifier);
    deviceDoc["name"] = deviceName;
    deviceDoc["model"] = "ORVIBO CT30W";
    deviceDoc["manufacturer"] = "ORVIBO";
    deviceDoc["sw_version"] = "1.0.0";
    
    // 1. 二进制传感器 - 设备在线状态（使用较小的缓冲区）
    {
        DynamicJsonDocument statusConfig(512);
        char nameBuf[128];
        snprintf(nameBuf, sizeof(nameBuf), "%s 状态", deviceName);
        statusConfig["name"] = nameBuf;
        char uniqueIdBuf[64];
        snprintf(uniqueIdBuf, sizeof(uniqueIdBuf), "%s_status", deviceId);
        statusConfig["unique_id"] = uniqueIdBuf;
        statusConfig["state_topic"] = MQTT_TOPIC_STATE "/status";
        statusConfig["device_class"] = "connectivity";
        statusConfig["payload_on"] = "online";
        statusConfig["payload_off"] = "offline";
        statusConfig["device"] = deviceDoc;
        
        char statusTopic[128];
        snprintf(statusTopic, sizeof(statusTopic), "%s/binary_sensor/%s/status/config", 
                 MQTT_DISCOVERY_PREFIX, deviceId);
        String statusPayload;
        serializeJson(statusConfig, statusPayload);
        Serial.printf("[HA Discovery] 二进制传感器消息大小: %d bytes (主题: %d bytes)\n", 
                      statusPayload.length(), strlen(statusTopic));
        if (mqtt.publish(statusTopic, statusPayload, true)) {
            Serial.printf("[HA Discovery] ✓ 二进制传感器配置已发布: %s\n", statusTopic);
        } else {
            Serial.printf("[HA Discovery] ✗ 二进制传感器配置发布失败: %s\n", statusTopic);
            Serial.printf("[HA Discovery] 总消息大小: %d bytes (可能超过缓冲区限制)\n", 
                          strlen(statusTopic) + statusPayload.length());
            allPublished = false;
        }
    }  // statusConfig在这里销毁，释放内存
    
    // 检查内存，如果不足则跳过后续发布
    freeHeap = ESP.getFreeHeap();
    if (freeHeap < 2048) {
        Serial.printf("[HA Discovery] 内存不足，跳过后续发布 (空闲: %d bytes)\n", freeHeap);
        if (allPublished) {
            discoveryPublished = true;
        }
        return;
    }
    
    // 2. 传感器 - 设备状态信息（使用较小的缓冲区）
    {
        DynamicJsonDocument stateConfig(512);
        char nameBuf[128];
        snprintf(nameBuf, sizeof(nameBuf), "%s 信息", deviceName);
        stateConfig["name"] = nameBuf;
        char uniqueIdBuf[64];
        snprintf(uniqueIdBuf, sizeof(uniqueIdBuf), "%s_state", deviceId);
        stateConfig["unique_id"] = uniqueIdBuf;
        stateConfig["state_topic"] = MQTT_TOPIC_STATE;
        stateConfig["value_template"] = "{{ value_json.status }}";
        stateConfig["json_attributes_topic"] = MQTT_TOPIC_STATE;
        stateConfig["json_attributes_template"] = "{{ value_json | tojson }}";
        stateConfig["device"] = deviceDoc;
        
        char stateTopic[128];
        snprintf(stateTopic, sizeof(stateTopic), "%s/sensor/%s/state/config", 
                 MQTT_DISCOVERY_PREFIX, deviceId);
        String statePayload;
        serializeJson(stateConfig, statePayload);
        Serial.printf("[HA Discovery] 传感器消息大小: %d bytes (主题: %d bytes)\n", 
                      statePayload.length(), strlen(stateTopic));
        if (mqtt.publish(stateTopic, statePayload, true)) {
            Serial.printf("[HA Discovery] ✓ 传感器配置已发布: %s\n", stateTopic);
        } else {
            Serial.printf("[HA Discovery] ✗ 传感器配置发布失败: %s\n", stateTopic);
            Serial.printf("[HA Discovery] 总消息大小: %d bytes (可能超过缓冲区限制)\n", 
                          strlen(stateTopic) + statePayload.length());
            allPublished = false;
        }
    }  // stateConfig在这里销毁，释放内存
    
    // 检查内存，如果不足则跳过后续发布
    freeHeap = ESP.getFreeHeap();
    if (freeHeap < 2048) {
        Serial.printf("[HA Discovery] 内存不足，跳过后续发布 (空闲: %d bytes)\n", freeHeap);
        if (allPublished) {
            discoveryPublished = true;
        }
        return;
    }
    
    // 3. 文本传感器 - 日志（使用较小的缓冲区）
    {
        DynamicJsonDocument logConfig(512);
        char nameBuf[128];
        snprintf(nameBuf, sizeof(nameBuf), "%s 日志", deviceName);
        logConfig["name"] = nameBuf;
        char uniqueIdBuf[64];
        snprintf(uniqueIdBuf, sizeof(uniqueIdBuf), "%s_log", deviceId);
        logConfig["unique_id"] = uniqueIdBuf;
        logConfig["state_topic"] = MQTT_TOPIC_LOG;
        logConfig["device"] = deviceDoc;
        
        char logTopic[128];
        snprintf(logTopic, sizeof(logTopic), "%s/sensor/%s/log/config", 
                 MQTT_DISCOVERY_PREFIX, deviceId);
        String logPayload;
        serializeJson(logConfig, logPayload);
        Serial.printf("[HA Discovery] 日志传感器消息大小: %d bytes (主题: %d bytes)\n", 
                      logPayload.length(), strlen(logTopic));
        if (mqtt.publish(logTopic, logPayload, true)) {
            Serial.printf("[HA Discovery] ✓ 日志传感器配置已发布: %s\n", logTopic);
        } else {
            Serial.printf("[HA Discovery] ✗ 日志传感器配置发布失败: %s\n", logTopic);
            Serial.printf("[HA Discovery] 总消息大小: %d bytes (可能超过缓冲区限制)\n", 
                          strlen(logTopic) + logPayload.length());
            allPublished = false;
        }
    }  // logConfig在这里销毁，释放内存
    
    if (allPublished) {
        discoveryPublished = true;
        Serial.println("[HA Discovery] ✓ 所有 Discovery 配置已成功发布");
        logger.log("mqtt", "Home Assistant Discovery 配置已发布");
        
        // 发布初始状态（检查内存）
        if (ESP.getFreeHeap() >= 1024) {
            char statusTopic[128];
            snprintf(statusTopic, sizeof(statusTopic), "%s/status", MQTT_TOPIC_STATE);
            mqtt.publish(statusTopic, "online", true);
            publishState("ready", "设备已就绪");
        }
    } else {
        Serial.println("[HA Discovery] ✗ 部分配置发布失败，将在下次重试");
    }
}

void HAMQTTHandler::processCommand(const String& payload) {
    // 内存保护：检查可用内存
    if (ESP.getFreeHeap() < 2048) {
        Serial.println("[HA MQTT] 内存不足，跳过命令处理");
        return;
    }
    
    // 优化：根据payload长度动态计算JSON大小
    size_t jsonSize = payload.length() + 512;
    if (jsonSize > 3072) jsonSize = 3072; // 限制最大3KB
    if (jsonSize < 512) jsonSize = 512;
    
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error) {
        // 优化：避免创建临时String对象
        char errorMsg[64];
        snprintf(errorMsg, sizeof(errorMsg), "Invalid JSON: %s", error.c_str());
        publishState("error", errorMsg);
        return;
    }
    
    String action = doc["action"] | "";
    
    if (action == "query_maps") {
        handleQueryMaps();
    } else if (action == "update_map") {
        handleUpdateMap(payload);
    } else if (action == "update_maps") {
        handleUpdateMaps(payload);
    } else if (action == "get_logs") {
        handleGetLogs();
    } else if (action == "upload_signal") {
        handleUploadSignal(payload);
    } else if (action == "stop_learn") {
        handleStopLearn(payload);
    } else if (action == "query_learn_result") {
        handleQueryLearnResult(payload);
    } else if (action == "get_recent_signals") {
        handleGetRecentSignals(payload);
    } else if (action == "send_signal") {
        handleSendSignal(payload);
    } else if (action == "set_forwarding_enabled") {
        handleSetForwardingEnabled(payload);
    } else {
        publishState("error", "Unknown action: " + action);
    }
}

void HAMQTTHandler::handleQueryMaps() {
    // 内存保护：检查可用内存
    if (ESP.getFreeHeap() < 2048) {
        publishState("error", "Low Memory");
        return;
    }
    
    IR_MapRule rules[MAX_MAP_RULES];
    int count = mapper.getRuleCount();
    mapper.getAllRules(rules, count);
    
    // 优化：更精确的JSON大小计算
    size_t jsonSize = 256; // 基础开销
    for (int i = 0; i < count; i++) {
        jsonSize += 120; // 规则基础开销
        jsonSize += rules[i].bSignalCount * 40;
        jsonSize += (rules[i].bSignalCount > 0 ? rules[i].bSignalCount - 1 : 0) * 4;
    }
    if (jsonSize > 8192) jsonSize = 8192; // 限制最大8KB
    if (jsonSize < 512) jsonSize = 512;
    
    // 检查是否有足够内存
    if (ESP.getFreeHeap() < jsonSize + 1024) {
        publishState("error", "Insufficient Memory");
        return;
    }
    
    DynamicJsonDocument doc(jsonSize);
    JsonArray rulesArray = doc.to<JsonArray>();
    
    for (int i = 0; i < count; i++) {
        JsonObject ruleObj = rulesArray.createNestedObject();
        ruleObj["id"] = rules[i].id;
        
        JsonObject aSignal = ruleObj.createNestedObject("aSignal");
        aSignal["protocol"] = rules[i].aSignal.protocol;
        aSignal["data"] = rules[i].aSignal.data;
        aSignal["hash"] = rules[i].aSignal.hash;
        aSignal["isProtocol"] = rules[i].aSignal.isProtocol;
        
        JsonArray bSignals = ruleObj.createNestedArray("bSignals");
        for (int j = 0; j < rules[i].bSignalCount; j++) {
            JsonObject bSignal = bSignals.createNestedObject();
            bSignal["protocol"] = rules[i].bSignals[j].protocol;
            bSignal["data"] = rules[i].bSignals[j].data;
            bSignal["hash"] = rules[i].bSignals[j].hash;
            bSignal["isProtocol"] = rules[i].bSignals[j].isProtocol;
        }
        
        ruleObj["bSignalCount"] = rules[i].bSignalCount;
        
        JsonArray intervals = ruleObj.createNestedArray("intervals");
        for (int j = 0; j < rules[i].bSignalCount - 1; j++) {
            intervals.add(rules[i].intervals[j]);
        }
        
        ruleObj["repeatCount"] = rules[i].repeatCount;
    }
    
    String output;
    serializeJson(doc, output);
    publishState("success", output);
}

void HAMQTTHandler::handleUpdateMap(const String& json) {
    // 内存保护：检查可用内存
    if (ESP.getFreeHeap() < 2048) {
        publishState("error", "Low Memory");
        return;
    }
    
    // 优化：根据json长度动态计算JSON大小
    size_t jsonSize = json.length() + 512;
    if (jsonSize > 3072) jsonSize = 3072;
    if (jsonSize < 512) jsonSize = 512;
    
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, json);
    
    if (error) {
        publishState("error", "Invalid JSON");
        return;
    }
    
    uint8_t id = doc["id"] | 0;
    if (id == 0) {
        publishState("error", "Invalid rule ID");
        return;
    }
    
    JsonObject ruleObj = doc["rule"];
    if (ruleObj.isNull()) {
        publishState("error", "Missing 'rule' field");
        return;
    }
    
    IR_MapRule rule;
    
    JsonObject aSignal = ruleObj["aSignal"];
    rule.aSignal.protocol = aSignal["protocol"] | 0;
    rule.aSignal.data = aSignal["data"] | 0;
    rule.aSignal.hash = aSignal["hash"] | 0;
    rule.aSignal.isProtocol = aSignal["isProtocol"] | false;
    
    JsonArray bSignals = ruleObj["bSignals"];
    rule.bSignalCount = 0;
    for (JsonObject bSignal : bSignals) {
        if (rule.bSignalCount >= MAX_B_SIGNALS_PER_RULE) break;
        
        rule.bSignals[rule.bSignalCount].protocol = bSignal["protocol"] | 0;
        rule.bSignals[rule.bSignalCount].data = bSignal["data"] | 0;
        rule.bSignals[rule.bSignalCount].hash = bSignal["hash"] | 0;
        rule.bSignals[rule.bSignalCount].isProtocol = bSignal["isProtocol"] | false;
        rule.bSignalCount++;
    }
    
    JsonArray intervals = ruleObj["intervals"];
    int intervalCount = 0;
    for (uint16_t interval : intervals) {
        if (intervalCount >= MAX_B_SIGNALS_PER_RULE - 1) break;
        rule.intervals[intervalCount++] = interval;
    }
    
    rule.repeatCount = ruleObj["repeatCount"] | 1;
    rule.valid = true;
    
    if (mapper.updateRule(id, rule)) {
        publishState("success", "Rule updated");
    } else {
        publishState("error", "Failed to update rule");
    }
}

void HAMQTTHandler::handleGetLogs() {
    // 内存保护：检查可用内存
    if (ESP.getFreeHeap() < 1536) {
        publishState("error", "Low Memory");
        return;
    }
    
    LogEntry entries[MAX_LOG_ENTRIES];
    int count = logger.getLogCount();
    logger.getLogs(entries, count);
    
    // 优化：更精确的JSON大小计算
    size_t jsonSize = 256;
    for (int i = 0; i < count; i++) {
        jsonSize += 108;
        jsonSize += strlen(entries[i].type);
        jsonSize += strlen(entries[i].content);
    }
    if (jsonSize > 4096) jsonSize = 4096;
    if (jsonSize < 512) jsonSize = 512;
    
    // 检查是否有足够内存
    if (ESP.getFreeHeap() < jsonSize + 1024) {
        publishState("error", "Insufficient Memory");
        return;
    }
    
    DynamicJsonDocument doc(jsonSize);
    JsonArray logArray = doc.to<JsonArray>();
    
    for (int i = 0; i < count; i++) {
        JsonObject entry = logArray.createNestedObject();
        entry["timestamp"] = entries[i].timestamp;
        entry["type"] = entries[i].type;
        entry["content"] = entries[i].content;
    }
    
    String output;
    serializeJson(doc, output);
    publishState("success", output);
}

void HAMQTTHandler::handleUploadSignal(const String& json) {
    // 上传自定义B信号序列
    // 实现逻辑类似添加映射规则，但用于上传信号数据
    publishState("success", "Signal uploaded");
}

void HAMQTTHandler::handleStopLearn(const String& json) {
    if (learner == nullptr) {
        publishState("error", "Learner service not initialized");
        return;
    }
    
    if (learner->stopDeviceLearn()) {
        publishState("success", "Device learn stopped");
    } else {
        publishState("error", "Device learn is not active");
    }
}

void HAMQTTHandler::handleQueryLearnResult(const String& json) {
    if (learner == nullptr) {
        publishState("error", "Learner service not initialized");
        return;
    }
    
    LearnedDevice device;
    if (!learner->getLearnedDevice(device)) {
        publishState("error", "No learned device found");
        return;
    }
    
    String smartIRJson = learner->convertToSmartIRFormat();
    publishState("success", smartIRJson);
}

void HAMQTTHandler::handleGetRecentSignals(const String& json) {
    RecentSignal signals[MAX_RECENT_SIGNALS];
    int count = logger.getRecentSignals(signals, MAX_RECENT_SIGNALS);
    
    DynamicJsonDocument doc(2048);
    JsonArray signalsArray = doc.to<JsonArray>();
    
    for (int i = 0; i < count; i++) {
        JsonObject signalObj = signalsArray.createNestedObject();
        signalObj["timestamp"] = signals[i].timestamp;
        signalObj["matched"] = signals[i].matched;
        
        JsonObject signal = signalObj.createNestedObject("signal");
        signal["protocol"] = signals[i].signal.protocol;
        signal["data"] = signals[i].signal.data;
        signal["hash"] = signals[i].signal.hash;
        signal["isProtocol"] = signals[i].signal.isProtocol;
    }
    
    String output;
    serializeJson(doc, output);
    publishState("success", output);
}

void HAMQTTHandler::handleSendSignal(const String& json) {
    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, json);
    
    if (error) {
        publishState("error", "Invalid JSON");
        return;
    }
    
    JsonObject signalObj = doc["signal"];
    if (signalObj.isNull()) {
        publishState("error", "Missing 'signal' field");
        return;
    }
    
    SignalID signal;
    signal.protocol = signalObj["protocol"] | 0;
    signal.data = signalObj["data"] | 0;
    signal.hash = signalObj["hash"] | 0;
    signal.isProtocol = signalObj["isProtocol"] | false;
    
    uint8_t repeatCount = doc["repeatCount"] | 1;
    
    if (forwarder.sendSignal(signal, repeatCount)) {
        publishState("executing", "Signal sent");
    } else {
        publishState("error", "Failed to send signal");
    }
}

void HAMQTTHandler::handleSetForwardingEnabled(const String& json) {
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, json);
    
    if (error) {
        publishState("error", "Invalid JSON");
        return;
    }
    
    bool enabled = doc["enabled"] | true;
    forwarder.setEnabled(enabled);
    
    DynamicJsonDocument response(128);
    response["success"] = true;
    response["message"] = "Forwarding " + String(enabled ? "enabled" : "disabled");
    String output;
    serializeJson(response, output);
    publishState("success", output);
}

void HAMQTTHandler::handleUpdateMaps(const String& json) {
    // 内存保护：检查可用内存（批量更新需要更多内存）
    if (ESP.getFreeHeap() < 4096) {
        publishState("error", "Low Memory");
        return;
    }
    
    // 优化：根据json长度动态计算JSON大小
    size_t jsonSize = json.length() + 1024;
    if (jsonSize > 6144) jsonSize = 6144; // 限制最大6KB
    if (jsonSize < 1024) jsonSize = 1024;
    
    // 检查是否有足够内存
    if (ESP.getFreeHeap() < jsonSize + 2048) {
        publishState("error", "Insufficient Memory");
        return;
    }
    
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, json);
    
    if (error) {
        publishState("error", "Invalid JSON");
        return;
    }
    
    if (!doc.containsKey("rules")) {
        publishState("error", "Missing 'rules' array");
        return;
    }
    
    JsonArray rulesArray = doc["rules"];
    
    // 清空所有现有规则
    IR_MapRule rules[MAX_MAP_RULES];
    int count = mapper.getRuleCount();
    mapper.getAllRules(rules, count);
    for (int i = 0; i < count; i++) {
        mapper.deleteRule(rules[i].id);
    }
    
    // 添加新规则
    int successCount = 0;
    for (JsonObject ruleObj : rulesArray) {
        IR_MapRule rule;
        
        JsonObject aSignal = ruleObj["aSignal"];
        rule.aSignal.protocol = aSignal["protocol"] | 0;
        rule.aSignal.data = aSignal["data"] | 0;
        rule.aSignal.hash = aSignal["hash"] | 0;
        rule.aSignal.isProtocol = aSignal["isProtocol"] | false;
        
        JsonArray bSignals = ruleObj["bSignals"];
        rule.bSignalCount = 0;
        for (JsonObject bSignal : bSignals) {
            if (rule.bSignalCount >= MAX_B_SIGNALS_PER_RULE) break;
            
            rule.bSignals[rule.bSignalCount].protocol = bSignal["protocol"] | 0;
            rule.bSignals[rule.bSignalCount].data = bSignal["data"] | 0;
            rule.bSignals[rule.bSignalCount].hash = bSignal["hash"] | 0;
            rule.bSignals[rule.bSignalCount].isProtocol = bSignal["isProtocol"] | false;
            rule.bSignalCount++;
        }
        
        JsonArray intervals = ruleObj["intervals"];
        int intervalCount = 0;
        for (uint16_t interval : intervals) {
            if (intervalCount >= MAX_B_SIGNALS_PER_RULE - 1) break;
            rule.intervals[intervalCount++] = interval;
        }
        
        rule.repeatCount = ruleObj["repeatCount"] | 1;
        rule.valid = true;
        
        if (mapper.addRule(rule)) {
            successCount++;
        }
    }
    
    DynamicJsonDocument response(256);
    response["success"] = true;
    response["message"] = "Rules updated";
    response["count"] = successCount;
    String output;
    serializeJson(response, output);
    publishState("success", output);
}

void HAMQTTHandler::publishEnhancedState() {
    // 检查可用内存
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 2048) {  // 至少需要2KB空闲内存
        return;  // 内存不足，跳过发布
    }
    
    // 使用较小的缓冲区，根据实际需要动态调整
    DynamicJsonDocument doc(1024);
    
    // 主状态
    String status = "ready";
    if (learner != nullptr && learner->isLearningDevice()) {
        status = "learning";
    }
    doc["status"] = status;
    
    // 转发状态
    doc["forwarding_enabled"] = forwarder.isEnabled();
    
    // 学习状态
    if (learner != nullptr) {
        if (learner->isLearningDevice()) {
            doc["learn_mode"] = "device";
            doc["learn_progress"] = learner->getLearnProgress();
            LearnedDevice device;
            if (learner->getLearnedDevice(device)) {
                doc["learn_device"] = device.deviceName;
            } else {
                doc["learn_device"] = "";
            }
        } else {
            doc["learn_mode"] = "none";
            doc["learn_progress"] = "";
            doc["learn_device"] = "";
        }
    } else {
        doc["learn_mode"] = "none";
        doc["learn_progress"] = "";
        doc["learn_device"] = "";
    }
    
    // 映射规则数量
    doc["rule_count"] = mapper.getRuleCount();
    
    // 最近接收的信号（只在内存充足时添加）
    if (freeHeap >= 3072) {
        RecentSignal signals[MAX_RECENT_SIGNALS];
        int signalCount = logger.getRecentSignals(signals, 1);  // 只获取最近1条
        if (signalCount > 0) {
            JsonObject recentSignal = doc.createNestedObject("recent_signal");
            recentSignal["timestamp"] = signals[0].timestamp;
            recentSignal["matched"] = signals[0].matched;
            
            JsonObject signal = recentSignal.createNestedObject("signal");
            signal["protocol"] = signals[0].signal.protocol;
            signal["data"] = signals[0].signal.data;
            signal["hash"] = signals[0].signal.hash;
            signal["isProtocol"] = signals[0].signal.isProtocol;
        }
    }
    
    doc["timestamp"] = millis();
    
    String output;
    serializeJson(doc, output);
    mqtt.publish(MQTT_TOPIC_STATE, output);
}

void HAMQTTHandler::mqttCallback(char* topic, byte* payload, unsigned int length) {
    if (instance == nullptr) return;
    
    String topicStr = String(topic);
    String payloadStr = "";
    for (unsigned int i = 0; i < length; i++) {
        payloadStr += (char)payload[i];
    }
    
    instance->handleCommand(topicStr, payloadStr);
}

