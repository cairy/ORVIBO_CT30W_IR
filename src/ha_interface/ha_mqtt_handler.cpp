#include "ha_mqtt_handler.h"
#include "ir_core/ir_learner.h"
#include "utils/hex_utils.h"
#include "utils/memory_monitor.h"
#include "utils/api_response.h"
#include "utils/unified_api.h"
#include "utils/api_handlers.h"
#include "config/constants.h"
#include <ArduinoJson.h>

HAMQTTHandler* HAMQTTHandler::instance = nullptr;

HAMQTTHandler::HAMQTTHandler(MQTTClient& mqtt, IRMapper& mapper,
                             IRForwarder& forwarder, Logger& logger)
    : mqtt(mqtt), mapper(mapper), forwarder(forwarder), logger(logger), 
      learner(nullptr), discoveryPublished(false), 
      lastDiscoveryAttempt(0), firstConnectedTime(0), discoveryRetryCount(0),
      lastStatePublishTime(0), lastPublishedForwardingEnabled(false),
      lastPublishedRuleCount(0), stateCacheInitialized(false) {
    // 初始化状态缓存数组
    lastPublishedStatus[0] = '\0';
    lastPublishedLearnMode[0] = '\0';
    instance = this;
}

bool HAMQTTHandler::init() {
    mqtt.setCallback(mqttCallback);
    
    // 设置日志推送回调（事件驱动模式）
    logger.setLogPublishCallback([](const char* type, const char* content) {
        if (instance != nullptr) {
            // 格式化日志条目（与HA传感器格式一致）
            char logStr[128];
            snprintf(logStr, sizeof(logStr), "[%s] %s", type, content);
            instance->publishLog(String(logStr));
        }
    });
    
    // 订阅将在 MQTT 连接成功后进行
    return true;
}

void HAMQTTHandler::update() {
    mqtt.update();
    
    // 当 MQTT 连接成功时
    static bool wasConnected = false;
    bool isConnected = mqtt.isConnected();
    
    if (isConnected && !wasConnected) {
        // 刚连接成功，订阅命令主题和 SmartIR 主题
        // 检查内存后再操作
        if (MemoryMonitor::checkMemory(2048)) {
            DEBUG_PRINTLN("[HA MQTT] MQTT 已连接，订阅命令主题...");
            // 喂看门狗，避免订阅过程中超时
            ESP.wdtFeed();
            if (mqtt.subscribe(MQTT_TOPIC_COMMAND)) {
                DEBUG_PRINTLN("[HA MQTT] ✓ 已订阅命令主题");
            } else {
                DEBUG_PRINTLN("[HA MQTT] ✗ 订阅失败");
            }
            
            // 订阅 SmartIR 主题（直接接收 Base64 字符串）
            DEBUG_PRINTLN("[HA MQTT] 订阅 SmartIR 主题...");
            mqtt.subscribe("smartir/climate/+/send");
            mqtt.subscribe("smartir/media_player/+/send");
            mqtt.subscribe("smartir/fan/+/send");
            mqtt.subscribe("smartir/light/+/send");
            ESP.wdtFeed();  // 订阅完成后再次喂看门狗
        } else {
            DEBUG_PRINTF("[HA MQTT] 内存不足，跳过订阅 (空闲: %d bytes)\n", ESP.getFreeHeap());
        }
        wasConnected = true;
    } else if (!isConnected && wasConnected) {
        // 连接断开
        DEBUG_PRINTLN("[HA MQTT] MQTT 连接已断开");
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
    
    // 首次连接时发布状态（事件驱动：只在未初始化时发布一次）
    if (isConnected && discoveryPublished && !stateCacheInitialized) {
        publishEnhancedState();
    }
}

void HAMQTTHandler::handleCommand(const String& topic, const String& payload) {
    // 检查是否是 SmartIR 主题（直接发送 Base64 字符串）
    if (topic.startsWith("smartir/") && topic.endsWith("/send")) {
        // SmartIR 直接发送 Base64 字符串，不需要 JSON 包装
        handleSmartIRSignal(payload);
        return;
    }
    
    // 处理设备命令主题
    if (topic == MQTT_TOPIC_COMMAND) {
        processCommand(payload);
    }
}

void HAMQTTHandler::handleCommand(const char* topic, const char* payload, size_t payloadLen) {
    // 优化：使用C字符串操作，避免String对象
    // 检查是否是 SmartIR 主题（直接发送 Base64 字符串）
    if (strncmp(topic, "smartir/", 8) == 0 && strstr(topic, "/send") != nullptr) {
        // SmartIR 直接发送 Base64 字符串，不需要 JSON 包装
        String payloadStr = String(payload);  // 仅在需要时创建String
        handleSmartIRSignal(payloadStr);
        return;
    }
    
    // 处理设备命令主题
    if (strcmp(topic, MQTT_TOPIC_COMMAND) == 0) {
        String payloadStr = String(payload);  // 仅在需要时创建String
        processCommand(payloadStr);
    }
}

void HAMQTTHandler::publishState(const String& state, const String& message) {
    // 优化：避免在MQTT回调中直接调用publishEnhancedState()，可能导致栈溢出
    // 如果内存充足，直接发布简单状态；否则延迟到下次update()
    if (MemoryMonitor::checkMemory(2048) && mqtt.isConnected()) {
        // 使用统一格式
        String output;
        if (state == "success") {
            output = UnifiedAPI::createSuccessResponse(message, "success");
        } else if (state == "error") {
            output = UnifiedAPI::createErrorResponse(message);
        } else if (state == "executing") {
            output = UnifiedAPI::createExecutingResponse(message);
        } else {
            // 兼容旧格式，但使用统一结构
            DynamicJsonDocument doc(256);
            doc["success"] = true;
            doc["status"] = state;
            doc["data"] = message;
            UnifiedAPI::addTimestamp(doc);
            serializeJson(doc, output);
        }
        mqtt.publish(MQTT_TOPIC_STATE, output);
    }
    // 如果内存不足，跳过发布（状态将在下次update()时通过publishEnhancedState()发布）
}

void HAMQTTHandler::publishLog(const String& logEntry) {
    // 优化：检查内存和连接状态
    if (!mqtt.isConnected() || !discoveryPublished) {
        return;
    }
    
    // 内存检查：日志推送需要较少内存（约512字节）
    if (!MemoryMonitor::checkMemory(512)) {
        return;  // 内存不足，跳过推送
    }
    
    // 发布到日志主题，更新 Home Assistant 日志传感器状态
    mqtt.publish(MQTT_TOPIC_LOG, logEntry);
}

void HAMQTTHandler::publishDiscoveryConfig() {
    if (discoveryPublished) return;
    
    if (!mqtt.isConnected()) {
        DEBUG_PRINTLN("[HA Discovery] MQTT 未连接，无法发布 Discovery 配置");
        return;
    }
    
    // 检查可用内存，如果内存不足则跳过发布
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 4096) {  // 至少需要4KB空闲内存
        DEBUG_PRINTF("[HA Discovery] 内存不足，跳过发布 (空闲: %d bytes)\n", freeHeap);
        return;
    }
    
    DEBUG_PRINTLN("[HA Discovery] 开始发布 Home Assistant Discovery 配置...");
    DEBUG_PRINTF("[HA Discovery] MQTT 缓冲区大小: %d bytes, 空闲内存: %d bytes\n", 
                  MQTT_BUFFER_SIZE, freeHeap);
    
    // 喂看门狗，避免发布过程中超时
    ESP.wdtFeed();
    
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
        DEBUG_PRINTF("[HA Discovery] 二进制传感器消息大小: %d bytes\n", statusPayload.length());
        if (mqtt.publish(statusTopic, statusPayload, true)) {
            DEBUG_PRINTLN("[HA Discovery] ✓ 二进制传感器配置已发布");
        } else {
            DEBUG_PRINTLN("[HA Discovery] ✗ 二进制传感器配置发布失败");
            allPublished = false;
        }
    }  // statusConfig在这里销毁，释放内存
    
    // 喂看门狗，避免长时间阻塞
    ESP.wdtFeed();
    yield();
    
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
        DEBUG_PRINTF("[HA Discovery] 传感器消息大小: %d bytes\n", statePayload.length());
        if (mqtt.publish(stateTopic, statePayload, true)) {
            DEBUG_PRINTLN("[HA Discovery] ✓ 传感器配置已发布");
        } else {
            DEBUG_PRINTLN("[HA Discovery] ✗ 传感器配置发布失败");
            allPublished = false;
        }
    }  // stateConfig在这里销毁，释放内存
    
    // 喂看门狗，避免长时间阻塞
    ESP.wdtFeed();
    yield();
    
    // 检查内存，如果不足则跳过后续发布
    freeHeap = ESP.getFreeHeap();
    if (freeHeap < 2048) {
        DEBUG_PRINTF("[HA Discovery] 内存不足，跳过后续发布 (空闲: %d bytes)\n", freeHeap);
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
        logConfig["icon"] = "mdi:text-box";
        logConfig["device"] = deviceDoc;
        
        char logTopic[128];
        snprintf(logTopic, sizeof(logTopic), "%s/sensor/%s/log/config", 
                 MQTT_DISCOVERY_PREFIX, deviceId);
        String logPayload;
        serializeJson(logConfig, logPayload);
        DEBUG_PRINTF("[HA Discovery] 日志传感器消息大小: %d bytes\n", logPayload.length());
        if (mqtt.publish(logTopic, logPayload, true)) {
            DEBUG_PRINTLN("[HA Discovery] ✓ 日志传感器配置已发布");
            // 发布初始日志状态
            if (MemoryMonitor::checkMemory(512)) {
                // 获取最新日志（通过 getLogs 获取所有日志，取最后一个）
                LogEntry entries[MAX_LOG_ENTRIES];
                String initialLog = "无日志";
                int logCount = logger.getLogCount();
                if (logCount > 0 && logger.getLogs(entries, logCount)) {
                    // 获取最后一个日志（最新的）
                    LogEntry latestEntry = entries[logCount - 1];
                    char logStr[128];
                    snprintf(logStr, sizeof(logStr), "[%s] %s", 
                            latestEntry.type, latestEntry.content);
                    initialLog = String(logStr);
                }
                mqtt.publish(MQTT_TOPIC_LOG, initialLog, true);
                DEBUG_PRINTLN("[HA Discovery] ✓ 日志传感器初始状态已发布");
            }
        } else {
            DEBUG_PRINTLN("[HA Discovery] ✗ 日志传感器配置发布失败");
            allPublished = false;
        }
    }  // logConfig在这里销毁，释放内存
    
    // 喂看门狗，避免长时间阻塞
    ESP.wdtFeed();
    yield();
    
    // 检查内存，如果不足则跳过后续发布
    freeHeap = ESP.getFreeHeap();
    if (freeHeap < 2048) {
        DEBUG_PRINTF("[HA Discovery] 内存不足，跳过后续发布 (空闲: %d bytes)\n", freeHeap);
        if (allPublished) {
            discoveryPublished = true;
        }
        return;
    }
    
    // 4. 开关 - 启用转发（使用较小的缓冲区）
    {
        DynamicJsonDocument forwardingSwitchConfig(512);
        char nameBuf[128];
        snprintf(nameBuf, sizeof(nameBuf), "%s 转发开关", deviceName);
        forwardingSwitchConfig["name"] = nameBuf;
        char uniqueIdBuf[64];
        snprintf(uniqueIdBuf, sizeof(uniqueIdBuf), "%s_forwarding_enabled", deviceId);
        forwardingSwitchConfig["unique_id"] = uniqueIdBuf;
        forwardingSwitchConfig["state_topic"] = MQTT_TOPIC_STATE;
        forwardingSwitchConfig["value_template"] = "{{ 'true' if value_json.data.forwarding_enabled else 'false' }}";
        forwardingSwitchConfig["command_topic"] = MQTT_TOPIC_COMMAND;
        forwardingSwitchConfig["payload_on"] = "true";
        forwardingSwitchConfig["payload_off"] = "false";
        forwardingSwitchConfig["state_on"] = "true";
        forwardingSwitchConfig["state_off"] = "false";
        forwardingSwitchConfig["icon"] = "mdi:swap-horizontal";
        forwardingSwitchConfig["device"] = deviceDoc;
        
        char forwardingSwitchTopic[128];
        snprintf(forwardingSwitchTopic, sizeof(forwardingSwitchTopic), "%s/switch/%s/forwarding_enabled/config", 
                 MQTT_DISCOVERY_PREFIX, deviceId);
        String forwardingSwitchPayload;
        serializeJson(forwardingSwitchConfig, forwardingSwitchPayload);
        DEBUG_PRINTF("[HA Discovery] 转发开关消息大小: %d bytes\n", forwardingSwitchPayload.length());
        if (mqtt.publish(forwardingSwitchTopic, forwardingSwitchPayload, true)) {
            DEBUG_PRINTLN("[HA Discovery] ✓ 转发开关配置已发布");
        } else {
            DEBUG_PRINTLN("[HA Discovery] ✗ 转发开关配置发布失败");
            allPublished = false;
        }
    }  // forwardingSwitchConfig在这里销毁，释放内存
    
    if (allPublished) {
        discoveryPublished = true;
        DEBUG_PRINTLN("[HA Discovery] ✓ 所有 Discovery 配置已成功发布");
        logger.log("mqtt", "Home Assistant Discovery 配置已发布");
        
        // 发布在线状态
        if (MemoryMonitor::checkMemory(1024)) {
            char statusTopic[128];
            snprintf(statusTopic, sizeof(statusTopic), "%s/status", MQTT_TOPIC_STATE);
            mqtt.publish(statusTopic, "online", true);
        }
        
        // Discovery发布成功后，发布初始设备状态（事件驱动）
        publishEnhancedState();
    } else {
        DEBUG_PRINTLN("[HA Discovery] ✗ 部分配置发布失败，将在下次重试");
    }
}

void HAMQTTHandler::processCommand(const String& payload) {
    // 使用统一的内存检查
    if (!MemoryMonitor::checkMemory(2048)) {
        DEBUG_PRINTLN("[HA MQTT] 内存不足，跳过命令处理");
        return;
    }
    
    // 检查是否是转发开关的命令（直接发送 true/false）
    String trimmedPayload = payload;
    trimmedPayload.trim();
    if (trimmedPayload == "true" || trimmedPayload == "false") {
        // 包装成JSON格式调用 handleSetForwardingEnabled()
        DynamicJsonDocument doc(128);
        doc["enabled"] = (trimmedPayload == "true");
        String jsonPayload;
        serializeJson(doc, jsonPayload);
        handleSetForwardingEnabled(jsonPayload);
        return;
    }
    
    // 优化：根据payload长度动态计算JSON大小，加强内存检查
    size_t jsonSize = payload.length() + 512;
    if (jsonSize > 3072) jsonSize = 3072; // 限制最大3KB
    if (jsonSize < 512) jsonSize = 512;
    
    // 加强内存检查：确保有足够的内存（JSON大小 + 安全缓冲）
    if (!MemoryMonitor::checkMemory(jsonSize + 1024)) {
        DEBUG_PRINTLN("[HA MQTT] 内存不足，跳过命令处理");
        return;
    }
    
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
    } else if (action == "get_logs") {
        handleGetLogs();
    } else if (action == "start_learn") {
        handleStartLearn(payload);
    } else if (action == "query_learn_status") {
        handleQueryLearnStatus(payload);
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
    } else if (action == "get_status") {
        handleGetStatus();
    } else {
        publishState("error", "Unknown action: " + action);
    }
}

void HAMQTTHandler::handleQueryMaps() {
    // 使用公共函数
    String dataJson = APIHandlers::handleQueryMaps(mapper);
    if (dataJson.length() == 0) {
        publishState("error", "Low Memory");
        return;
    }
    
    // 解析为JsonArray并包装为统一响应
    DynamicJsonDocument dataDoc(2048);
    DeserializationError err = deserializeJson(dataDoc, dataJson);
    if (err) {
        publishState("error", "Failed to parse rules");
        return;
    }
    
    JsonArray rulesArray = dataDoc.as<JsonArray>();
    String output = UnifiedAPI::createSuccessResponse(rulesArray, "success");
    mqtt.publish(MQTT_TOPIC_STATE, output);
}

void HAMQTTHandler::handleUpdateMap(const String& json) {
    // 使用公共函数
    bool success = false;
    String message = APIHandlers::handleUpdateMap(mapper, json, success);
    
    if (success) {
        publishState("success", message);
        publishEnhancedState();  // 规则数量可能变化，发布设备状态
    } else {
        publishState("error", message);
    }
}

void HAMQTTHandler::handleGetLogs() {
    // 使用公共函数（返回所有日志，系统只保留3条）
    String dataJson = APIHandlers::handleGetLogs(logger);
    if (dataJson.length() == 0) {
        publishState("error", "Low Memory");
        return;
    }
    
    // 解析为JsonArray并包装为统一响应
    DynamicJsonDocument dataDoc(1024);
    DeserializationError err = deserializeJson(dataDoc, dataJson);
    if (err) {
        publishState("error", "Failed to parse logs");
        return;
    }
    
    JsonArray logArray = dataDoc.as<JsonArray>();
    String output = UnifiedAPI::createSuccessResponse(logArray, "success");
    mqtt.publish(MQTT_TOPIC_STATE, output);
}

void HAMQTTHandler::handleStartLearn(const String& json) {
    if (learner == nullptr) {
        publishState("error", "Learner service not initialized");
        return;
    }
    
    // 使用公共函数
    bool success = false;
    String message = APIHandlers::handleStartLearn(*learner, json, success);
    
    if (success) {
        publishState("success", message);
        publishEnhancedState();  // 学习状态变化，发布设备状态
    } else {
        publishState("error", message);
    }
}

void HAMQTTHandler::handleQueryLearnStatus(const String& json) {
    if (learner == nullptr) {
        publishState("error", "Learner service not initialized");
        return;
    }
    
    // 使用公共函数（简化版）
    String dataJson = APIHandlers::handleQueryLearnStatus(*learner);
    if (dataJson.length() == 0) {
        publishState("error", "Low Memory");
        return;
    }
    
    // 解析为JsonObject并包装为统一响应
    DynamicJsonDocument dataDoc(512);
    DeserializationError err = deserializeJson(dataDoc, dataJson);
    if (err) {
        publishState("error", "Failed to parse learn status");
        return;
    }
    
    JsonObject dataObj = dataDoc.as<JsonObject>();
    String output = UnifiedAPI::createSuccessResponse(dataObj, "success");
    mqtt.publish(MQTT_TOPIC_STATE, output);
}

void HAMQTTHandler::handleStopLearn(const String& json) {
    if (learner == nullptr) {
        publishState("error", "Learner service not initialized");
        return;
    }
    
    // 使用公共函数
    bool success = false;
    String message = APIHandlers::handleStopLearn(*learner, success);
    
    if (success) {
        publishState("success", message);
        publishEnhancedState();  // 学习状态变化，发布设备状态
    } else {
        publishState("error", message);
    }
}

void HAMQTTHandler::handleQueryLearnResult(const String& json) {
    if (learner == nullptr) {
        publishState("error", "Learner service not initialized");
        return;
    }
    
    // 解析format参数（从JSON中读取）
    String format = "base64";  // 默认格式
    if (json.length() > 0) {
        size_t jsonSize = json.length() + 256;
        if (jsonSize > 512) jsonSize = 512;
        DynamicJsonDocument doc(jsonSize);
        DeserializationError err = deserializeJson(doc, json);
        if (!err && doc.containsKey("format")) {
            format = doc["format"] | "base64";
        }
    }
    
    // 使用公共函数
    String dataJson = APIHandlers::handleQueryLearnResult(*learner, format);
    if (dataJson.length() == 0) {
        publishState("error", "No learned device found");
        return;
    }
    
    // 尝试解析为JSON对象或数组，如果失败则作为字符串返回
    DynamicJsonDocument dataDoc(4096);
    DeserializationError err = deserializeJson(dataDoc, dataJson);
    
    if (err) {
        // 如果不是JSON，直接作为字符串返回
        String output = UnifiedAPI::createSuccessResponse(dataJson, "success");
        mqtt.publish(MQTT_TOPIC_STATE, output);
    } else {
        // 如果是JSON，提取为JsonObject或JsonArray
        if (dataDoc.is<JsonObject>()) {
            JsonObject obj = dataDoc.as<JsonObject>();
            String output = UnifiedAPI::createSuccessResponse(obj, "success");
            mqtt.publish(MQTT_TOPIC_STATE, output);
        } else if (dataDoc.is<JsonArray>()) {
            JsonArray arr = dataDoc.as<JsonArray>();
            String output = UnifiedAPI::createSuccessResponse(arr, "success");
            mqtt.publish(MQTT_TOPIC_STATE, output);
        } else {
            // 其他情况，作为字符串返回
            String output = UnifiedAPI::createSuccessResponse(dataJson, "success");
            mqtt.publish(MQTT_TOPIC_STATE, output);
        }
    }
}

void HAMQTTHandler::handleGetRecentSignals(const String& json) {
    // 使用公共函数
    String dataJson = APIHandlers::handleGetRecentSignals(logger);
    if (dataJson.length() == 0) {
        publishState("error", "Low Memory");
        return;
    }
    
    // 解析为JsonArray并包装为统一响应
    DynamicJsonDocument dataDoc(1536);
    DeserializationError err = deserializeJson(dataDoc, dataJson);
    if (err) {
        publishState("error", "Failed to parse signals");
        return;
    }
    
    JsonArray signalsArray = dataDoc.as<JsonArray>();
    String output = UnifiedAPI::createSuccessResponse(signalsArray, "success");
    mqtt.publish(MQTT_TOPIC_STATE, output);
}

void HAMQTTHandler::handleSendSignal(const String& payload) {
    String trimmed = payload;
    trimmed.trim();
    
    // 如果payload是JSON格式（包含action字段），提取实际的信号数据
    if (trimmed.startsWith("{") && trimmed.indexOf("\"action\"") >= 0) {
        size_t jsonSize = trimmed.length() + 256;
        if (jsonSize > 1024) jsonSize = 1024;
        
        // 加强内存检查：确保有足够的内存
        if (!MemoryMonitor::checkMemory(jsonSize + 512)) {
            DEBUG_PRINTLN("[HA MQTT] 内存不足，跳过信号处理");
            return;
        }
        
        DynamicJsonDocument doc(jsonSize);
        DeserializationError err = deserializeJson(doc, trimmed);
        if (!err && doc.containsKey("signal")) {
            // 提取signal字段，构建完整的UnifiedSignal格式JSON
            JsonObject signalObj = doc["signal"];
            uint8_t repeatCount = doc["repeatCount"] | 1;
            
            // 构建完整的UnifiedSignal格式JSON（包含signal和repeatCount）
            // 加强内存检查
            if (!MemoryMonitor::checkMemory(1024)) {
                DEBUG_PRINTLN("[HA MQTT] 内存不足，跳过信号处理");
                return;
            }
            
            DynamicJsonDocument signalDoc(512);
            signalDoc["signal"] = signalObj;
            signalDoc["repeatCount"] = repeatCount;
            
            String signalJson;
            serializeJson(signalDoc, signalJson);
            trimmed = signalJson;
        } else if (!err && doc.containsKey("action") && doc["action"] == "send_signal") {
            // 如果payload本身就是完整的send_signal命令，提取signal字段
            JsonObject signalObj = doc["signal"];
            if (!signalObj.isNull()) {
                uint8_t repeatCount = doc["repeatCount"] | 1;
                
                // 加强内存检查
                if (!MemoryMonitor::checkMemory(1024)) {
                    DEBUG_PRINTLN("[HA MQTT] 内存不足，跳过信号处理");
                    return;
                }
                
                // 构建完整的UnifiedSignal格式JSON
                DynamicJsonDocument signalDoc(512);
                signalDoc["signal"] = signalObj;
                signalDoc["repeatCount"] = repeatCount;
                
                String signalJson;
                serializeJson(signalDoc, signalJson);
                trimmed = signalJson;
            }
        }
    }
    
    // 使用公共函数（支持JSON和字符串格式）
    bool success = false;
    String message = APIHandlers::handleSendSignal(forwarder, trimmed, success);
    
    if (success) {
        publishState("executing", message);
    } else {
        publishState("error", message);
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
    
    // 使用统一格式
    String message = "Forwarding " + String(enabled ? "enabled" : "disabled");
    String output = UnifiedAPI::createSuccessResponse(message, "success");
    mqtt.publish(MQTT_TOPIC_STATE, output);
    
    // 转发状态变化，发布设备状态
    publishEnhancedState();
}

void HAMQTTHandler::handleSmartIRSignal(const String& payload) {
    // SmartIR 发送的格式（纯字符串）：Base64、数组、协议格式
    if (!MemoryMonitor::checkMemory(2048)) {
        DEBUG_PRINTLN("[SmartIR] 内存不足，跳过信号处理");
        return;
    }
    
    if (payload.length() == 0) {
        DEBUG_PRINTLN("[SmartIR] 收到空的 payload");
        return;
    }
    
    DEBUG_PRINTF("[SmartIR] 收到信号，长度: %d bytes\n", payload.length());
    
    // 使用统一的解析和发送方法
    if (forwarder.parseAndSendSignal(payload)) {
        DEBUG_PRINTLN("[SmartIR] 信号发送成功");
    } else {
        DEBUG_PRINTLN("[SmartIR] 信号发送失败");
    }
}

void HAMQTTHandler::handleGetStatus() {
    // 使用统一的内存检查
    if (!MemoryMonitor::checkMemory(2048)) {
        publishState("error", "Low Memory");
        return;
    }
    
    // 喂看门狗，避免发布过程中超时
    ESP.wdtFeed();
    
    // 获取当前状态值（复用 publishEnhancedState() 的逻辑）
    const char* currentStatus = "ready";
    if (learner != nullptr && learner->isLearningDevice()) {
        currentStatus = "learning";
    }
    
    bool currentForwardingEnabled = forwarder.isEnabled();
    
    const char* currentLearnMode = "none";
    if (learner != nullptr && learner->isLearningDevice()) {
        currentLearnMode = "device";
    }
    
    int currentRuleCount = mapper.getRuleCount();
    
    // 使用统一格式，但保留所有HA需要的字段
    DynamicJsonDocument doc(512);
    
    // 统一格式：success和status在顶层
    doc["success"] = true;
    doc["status"] = currentStatus;
    
    // 数据放在data对象中（HA需要的字段）
    JsonObject dataObj = doc.createNestedObject("data");
    dataObj["forwarding_enabled"] = currentForwardingEnabled;
    dataObj["learn_mode"] = currentLearnMode;
    
    // 学习状态（优化：避免在栈上创建大的LearnedDevice对象）
    if (learner != nullptr && learner->isLearningDevice()) {
        // 直接获取设备名称字符串，避免复制整个LearnedDevice结构体
        String deviceName = learner->getDeviceName();
        dataObj["learn_device"] = deviceName;
        deviceName = "";  // 立即释放
        
        String progress = learner->getLearnProgress();
        dataObj["learn_progress"] = progress;
        progress = "";  // 立即释放
    } else {
        dataObj["learn_progress"] = "";
        dataObj["learn_device"] = "";
    }
    
    // 映射规则数量
    dataObj["rule_count"] = currentRuleCount;
    
    UnifiedAPI::addTimestamp(doc);
    
    // 喂看门狗，避免序列化过程中超时
    ESP.wdtFeed();
    
    String output;
    serializeJson(doc, output);
    mqtt.publish(MQTT_TOPIC_STATE, output);
    
    // 注意：查询接口不更新状态缓存，因为这不是状态变化事件
    // 发布完成后再次喂看门狗
    ESP.wdtFeed();
}

void HAMQTTHandler::publishEnhancedState() {
    // 使用统一的内存检查
    if (!MemoryMonitor::checkMemory(2048)) {
        return;  // 内存不足，跳过发布
    }
    
    // 喂看门狗，避免发布过程中超时
    ESP.wdtFeed();
    
    // 获取当前状态值
    const char* currentStatus = "ready";
    if (learner != nullptr && learner->isLearningDevice()) {
        currentStatus = "learning";
    }
    
    bool currentForwardingEnabled = forwarder.isEnabled();
    
    const char* currentLearnMode = "none";
    if (learner != nullptr && learner->isLearningDevice()) {
        currentLearnMode = "device";
    }
    
    int currentRuleCount = mapper.getRuleCount();
    
    // 检查状态是否有变化（事件驱动）
    bool stateChanged = false;
    if (!stateCacheInitialized) {
        // 首次发布，立即发布
        stateChanged = true;
    } else {
        // 比较各个状态字段
        if (strcmp(currentStatus, lastPublishedStatus) != 0 ||
            currentForwardingEnabled != lastPublishedForwardingEnabled ||
            strcmp(currentLearnMode, lastPublishedLearnMode) != 0 ||
            currentRuleCount != lastPublishedRuleCount) {
            stateChanged = true;
        }
    }
    
    // 如果状态未变化且已初始化，跳过发布（节约资源）
    if (!stateChanged && stateCacheInitialized) {
        return;
    }
    
    // 使用统一格式，但保留所有HA需要的字段
    DynamicJsonDocument doc(512);
    
    // 统一格式：success和status在顶层
    doc["success"] = true;
    doc["status"] = currentStatus;
    
    // 数据放在data对象中（HA需要的字段）
    JsonObject dataObj = doc.createNestedObject("data");
    dataObj["forwarding_enabled"] = currentForwardingEnabled;
    dataObj["learn_mode"] = currentLearnMode;
    
    // 学习状态（优化：避免在栈上创建大的LearnedDevice对象）
    if (learner != nullptr && learner->isLearningDevice()) {
        // 直接获取设备名称字符串，避免复制整个LearnedDevice结构体
        String deviceName = learner->getDeviceName();
        dataObj["learn_device"] = deviceName;
        deviceName = "";  // 立即释放
        
        String progress = learner->getLearnProgress();
        dataObj["learn_progress"] = progress;
        progress = "";  // 立即释放
    } else {
        dataObj["learn_progress"] = "";
        dataObj["learn_device"] = "";
    }
    
    // 映射规则数量
    dataObj["rule_count"] = currentRuleCount;
    
    // 注意：最近信号信息已移除，用户可通过 get_recent_signals API接口查询
    // 移除自动获取以减少内存分配和JSON构建开销，提升转发性能
    
    UnifiedAPI::addTimestamp(doc);
    
    // 喂看门狗，避免序列化过程中超时
    ESP.wdtFeed();
    
    String output;
    serializeJson(doc, output);
    mqtt.publish(MQTT_TOPIC_STATE, output);
    
    // 更新状态缓存
    strncpy(lastPublishedStatus, currentStatus, sizeof(lastPublishedStatus) - 1);
    lastPublishedStatus[sizeof(lastPublishedStatus) - 1] = '\0';
    lastPublishedForwardingEnabled = currentForwardingEnabled;
    strncpy(lastPublishedLearnMode, currentLearnMode, sizeof(lastPublishedLearnMode) - 1);
    lastPublishedLearnMode[sizeof(lastPublishedLearnMode) - 1] = '\0';
    lastPublishedRuleCount = currentRuleCount;
    lastStatePublishTime = millis();
    stateCacheInitialized = true;
    
    // 发布完成后再次喂看门狗
    ESP.wdtFeed();
}

void HAMQTTHandler::mqttCallback(char* topic, byte* payload, unsigned int length) {
    if (instance == nullptr) return;
    
    // 优化：完全避免String对象，使用纯C字符串操作
    // 限制topic和payload的最大长度，避免栈溢出
    const size_t MAX_TOPIC_LEN = 128;
    const size_t MAX_PAYLOAD_LEN = 512;
    
    static char topicBuf[MAX_TOPIC_LEN];
    static char payloadBuf[MAX_PAYLOAD_LEN];
    
    // 复制topic（限制长度）
    size_t topicLen = strlen(topic);
    if (topicLen >= MAX_TOPIC_LEN) topicLen = MAX_TOPIC_LEN - 1;
    memcpy(topicBuf, topic, topicLen);
    topicBuf[topicLen] = '\0';
    
    // 复制payload（限制长度）
    size_t payloadLen = (length < MAX_PAYLOAD_LEN) ? length : MAX_PAYLOAD_LEN;
    memcpy(payloadBuf, payload, payloadLen);
    payloadBuf[payloadLen] = '\0';
    
    // 直接使用C字符串，避免String对象创建
    instance->handleCommand(topicBuf, payloadBuf, payloadLen);
}

