#include "web_server.h"
#include "ir_core/ir_mapper.h"
#include "ir_core/ir_forwarder.h"
#include "ir_core/ir_learner.h"
#include "utils/logger.h"
#include "config/constants.h"
#include <ArduinoJson.h>

WebServer::WebServer(NetworkConfigManager& config)
    : server(80), configManager(config), running(false),
      mapper(nullptr), forwarder(nullptr), learner(nullptr), logger(nullptr) {
}

bool WebServer::init() {
    // 配网界面路由
    server.on("/", HTTP_GET, [this]() { this->handleRoot(); });
    server.on("/config", HTTP_GET, [this]() { this->handleConfig(); });
    server.on("/save", HTTP_POST, [this]() { this->handleSave(); });
    
    // HTTP API路由
    server.on("/api/status", HTTP_GET, [this]() { this->handleAPIStatus(); });
    server.on("/api/maps", HTTP_GET, [this]() { this->handleAPIMaps(); });
    server.on("/api/maps", HTTP_PUT, [this]() { this->handleAPIUpdateMap(); });
    server.on("/api/logs", HTTP_GET, [this]() { this->handleAPILogs(); });
    server.on("/api/learn/start", HTTP_POST, [this]() { this->handleAPILearnStart(); });
    server.on("/api/learn/status", HTTP_GET, [this]() { this->handleAPILearnStatus(); });
    server.on("/api/learn/stop", HTTP_POST, [this]() { this->handleAPILearnStop(); });
    server.on("/api/learn/result", HTTP_GET, [this]() { this->handleAPILearnResult(); });
    server.on("/api/signals/recent", HTTP_GET, [this]() { this->handleAPIRecentSignals(); });
    server.on("/api/signals/send", HTTP_POST, [this]() { this->handleAPISendSignal(); });
    
    // 处理OPTIONS请求（CORS预检）
    server.onNotFound([this]() {
        if (server.method() == HTTP_OPTIONS) {
            this->addCORSHeaders();
            server.send(200, "text/plain", "");
        } else {
            server.send(404, "text/plain", "Not Found");
        }
    });
    
    return true;
}

void WebServer::handleClient() {
    server.handleClient();
}

void WebServer::stop() {
    if (running) {
        server.stop();
        running = false;
        Serial.println("Web server stopped");
    }
}

void WebServer::start() {
    if (!running) {
        server.begin();
        running = true;
        Serial.println("Web server started");
    }
}

bool WebServer::isRunning() const {
    return running;
}

void WebServer::handleRoot() {
    server.send(200, "text/html", getHTML());
}

void WebServer::handleConfig() {
    handleRoot();
}

void WebServer::handleSave() {
    String ssid = server.arg("ssid");
    String password = server.arg("password");
    String mqtt_server = server.arg("mqtt_server");
    String mqtt_port = server.arg("mqtt_port");
    String mqtt_username = server.arg("mqtt_username");
    String mqtt_password = server.arg("mqtt_password");
    
    if (ssid.length() > 0) {
        configManager.setWiFi(ssid, password);
    }
    
    // 如果MQTT服务器地址为空，清除MQTT配置；否则设置MQTT配置
    if (mqtt_server.length() == 0) {
        configManager.clearMQTT();
    } else {
        uint16_t port = mqtt_port.toInt();
        if (port == 0) port = 1883;
        configManager.setMQTT(mqtt_server, port, mqtt_username, mqtt_password);
    }
    
    configManager.save();
    
    // 使用F()宏将字符串存储在Flash中
    String response;
    response.reserve(200);
    response = F("<!DOCTYPE html><html><head><meta charset='UTF-8'>");
    response += F("<title>配置成功</title></head><body>");
    response += F("<h1>配置已保存</h1>");
    response += F("<p>设备将重启并连接到WiFi...</p>");
    response += F("</body></html>");
    
    server.send(200, "text/html", response);
    delay(1000);
    ESP.restart();
}

String WebServer::getHTML() {
    NetworkConfig& config = configManager.getConfig();
    bool mqttConfigured = configManager.isMQTTConfigured();
    
    // 简化：预分配固定大小，足够使用
    String html;
    html.reserve(1500);
    
    // 使用F()宏将字符串存储在Flash中，减少RAM占用
    html = F("<!DOCTYPE html><html><head><meta charset='UTF-8'>");
    html += F("<meta name='viewport' content='width=device-width, initial-scale=1.0'>");
    html += F("<title>ORVIBO-CT30W 配置</title>");
    html += F("<style>body{font-family:Arial;max-width:500px;margin:50px auto;padding:20px;}");
    html += F("input,button{width:100%;padding:10px;margin:5px 0;box-sizing:border-box;}");
    html += F("button{background:#4CAF50;color:white;border:none;cursor:pointer;}");
    html += F("button:hover{background:#45a049;}</style></head><body>");
    html += F("<h1>ORVIBO-CT30W 网络配置</h1>");
    html += F("<form action='/save' method='POST'>");
    html += F("<h2>WiFi 配置</h2>");
    html += F("<input type='text' name='ssid' placeholder='WiFi SSID' value='");
    html += config.wifi_ssid;
    html += F("' required>");
    // 密码字段不显示实际值，提高安全性
    html += F("<input type='password' name='password' placeholder='WiFi 密码' required>");
    html += F("<h2>MQTT 配置（可选）</h2>");
    html += F("<input type='text' name='mqtt_server' placeholder='MQTT 服务器地址（可选）' value='");
    // 如果MQTT未配置，显示空值
    if (mqttConfigured) {
        html += config.mqtt_server;
    }
    html += F("'>");
    html += F("<input type='number' name='mqtt_port' placeholder='MQTT 端口（默认1883）' value='");
    // 如果MQTT未配置，显示默认端口1883
    if (mqttConfigured) {
        html += config.mqtt_port;
    } else {
        html += F("1883");
    }
    html += F("'>");
    html += F("<input type='text' name='mqtt_username' placeholder='MQTT 用户名（可选）' value='");
    // 如果MQTT未配置，显示空值
    if (mqttConfigured) {
        html += config.mqtt_username;
    }
    html += F("'>");
    // MQTT密码字段不显示实际值，提高安全性
    html += F("<input type='password' name='mqtt_password' placeholder='MQTT 密码（可选）'>");
    html += F("<button type='submit'>保存配置</button>");
    html += F("</form></body></html>");
    
    return html;
}

// ========== HTTP API 处理方法 ==========

void WebServer::addCORSHeaders() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void WebServer::sendJSONResponse(int code, const String& json) {
    addCORSHeaders();
    server.send(code, "application/json", json);
}

void WebServer::sendErrorResponse(int code, const String& error) {
    DynamicJsonDocument doc(256);
    doc["success"] = false;
    doc["error"] = error;
    String output;
    output.reserve(256);
    serializeJson(doc, output);
    sendJSONResponse(code, output);
}

void WebServer::handleAPIStatus() {
    if (mapper == nullptr || forwarder == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    DynamicJsonDocument doc(256);
    doc["success"] = true;
    doc["status"] = "running";
    doc["ruleCount"] = mapper->getRuleCount();
    doc["forwarding"] = forwarder->isForwarding();
    
    String output;
    output.reserve(256);
    serializeJson(doc, output);
    sendJSONResponse(200, output);
}

void WebServer::handleAPIMaps() {
    if (mapper == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 检查内存：避免在栈上分配大数组导致栈溢出
    size_t freeHeap = ESP.getFreeHeap();
    int count = mapper->getRuleCount();
    
    // 估算所需内存：每个规则约108字节，加上JSON开销
    size_t estimatedMemory = count * 120;  // 规则数组 + 安全余量
    if (freeHeap < estimatedMemory + 2048) {
        sendErrorResponse(503, "Insufficient Memory");
        return;
    }
    
    // 优化：只分配实际需要的数量，而不是MAX_MAP_RULES
    // 限制最大数量，避免栈溢出
    int maxRules = (count < MAX_MAP_RULES) ? count : MAX_MAP_RULES;
    if (maxRules > 20) maxRules = 20;  // 进一步限制，避免栈溢出
    
    IR_MapRule rules[maxRules];
    if (!mapper->getAllRules(rules, maxRules)) {
        sendErrorResponse(500, "Failed to get rules");
        return;
    }
    
    // 简化：根据规则数量动态计算JSON大小
    size_t jsonSize = 256 + (maxRules * 200);  // 基础开销 + 每条规则约200字节
    if (jsonSize > 6144) jsonSize = 6144;   // 限制最大6KB
    if (jsonSize < 512) jsonSize = 512;
    
    DynamicJsonDocument doc(jsonSize);
    JsonArray rulesArray = doc.to<JsonArray>();
    
    for (int i = 0; i < maxRules; i++) {
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
    output.reserve(jsonSize);
    serializeJson(doc, output);
    sendJSONResponse(200, output);
}

void WebServer::handleAPIUpdateMap() {
    if (mapper == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    String body = server.arg("plain");
    if (body.length() == 0) {
        sendErrorResponse(400, "Missing request body");
        return;
    }
    
    // 简化：根据body长度动态计算JSON大小
    size_t jsonSize = body.length() + 512;
    if (jsonSize > 6144) jsonSize = 6144;
    if (jsonSize < 512) jsonSize = 512;
    
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, body);
    
    if (error) {
        // 优化：避免创建临时String对象
        char errorMsg[64];
        snprintf(errorMsg, sizeof(errorMsg), "Invalid JSON: %s", error.c_str());
        sendErrorResponse(400, errorMsg);
        return;
    }
    
    // 检查是否是批量更新（包含rules数组）
    if (doc.containsKey("rules")) {
        // 批量更新：清空现有规则，添加新规则
        JsonArray rulesArray = doc["rules"];
        
        // 清空所有现有规则（避免在栈上分配大数组导致栈溢出）
        // 使用小数组存储规则ID，逐个删除
        uint8_t ruleIds[20];  // 限制最多20个规则ID（每个ID 1字节，总共20字节）
        int idCount = 0;
        
        // 获取所有规则ID（使用较小的临时数组，避免栈溢出）
        int count = mapper->getRuleCount();
        if (count > 20) count = 20;  // 限制最多20个规则
        
        IR_MapRule tempRule;
        for (uint8_t id = 1; id <= 255 && idCount < count; id++) {
            if (mapper->getRule(id, tempRule)) {
                ruleIds[idCount++] = id;
            }
        }
        
        // 删除找到的所有规则
        for (int i = 0; i < idCount; i++) {
            mapper->deleteRule(ruleIds[i]);
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
            
            if (mapper->addRule(rule)) {
                successCount++;
            }
        }
        
        DynamicJsonDocument response(192);
        response["success"] = true;
        response["message"] = "Rules updated";
        response["count"] = successCount;
        String output;
        output.reserve(192);
        serializeJson(response, output);
        sendJSONResponse(200, output);
    } else {
        // 单个更新（保持向后兼容）
        uint8_t id = doc["id"] | 0;
        if (id == 0) {
            sendErrorResponse(400, "Invalid rule ID or missing 'rules' array");
            return;
        }
        
        JsonObject ruleObj = doc["rule"];
        if (ruleObj.isNull()) {
            sendErrorResponse(400, "Missing 'rule' field");
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
        
        if (mapper->updateRule(id, rule)) {
            DynamicJsonDocument response(128);
            response["success"] = true;
            response["message"] = "Rule updated";
            String output;
            output.reserve(128);
            serializeJson(response, output);
            sendJSONResponse(200, output);
        } else {
            sendErrorResponse(500, "Failed to update rule");
        }
    }
}

void WebServer::handleAPILogs() {
    if (logger == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    LogEntry entries[MAX_LOG_ENTRIES];
    int count = logger->getLogCount();
    logger->getLogs(entries, count);
    
    // 简化：根据日志数量计算JSON大小
    size_t jsonSize = 256 + (count * 100);
    if (jsonSize > 3072) jsonSize = 3072;
    if (jsonSize < 512) jsonSize = 512;
    
    DynamicJsonDocument doc(jsonSize);
    JsonArray logArray = doc.to<JsonArray>();
    
    for (int i = 0; i < count; i++) {
        JsonObject entry = logArray.createNestedObject();
        entry["timestamp"] = entries[i].timestamp;
        entry["type"] = entries[i].type;
        entry["content"] = entries[i].content;
    }
    
    String output;
    output.reserve(jsonSize);
    serializeJson(doc, output);
    sendJSONResponse(200, output);
}

void WebServer::handleAPILearnStart() {
    if (learner == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    String body = server.arg("plain");
    if (body.length() == 0) {
        sendErrorResponse(400, "Missing request body");
        return;
    }
    
    // 内存保护：检查可用内存
    if (ESP.getFreeHeap() < 2048) {
        sendErrorResponse(503, "Low Memory");
        return;
    }
    
    // 解析JSON
    size_t jsonSize = body.length() + 256;
    if (jsonSize > 1024) jsonSize = 1024;
    if (jsonSize < 256) jsonSize = 256;
    
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, body);
    
    if (error) {
        char errorMsg[64];
        snprintf(errorMsg, sizeof(errorMsg), "Invalid JSON: %s", error.c_str());
        sendErrorResponse(400, errorMsg);
        return;
    }
    
    String deviceName = doc["deviceName"] | "";
    String commandNames = doc["commandNames"] | "";
    int timeoutSeconds = doc["timeoutSeconds"] | DEFAULT_LEARN_TIMEOUT_SEC;
    
    if (deviceName.length() == 0) {
        sendErrorResponse(400, "Missing 'deviceName' field");
        return;
    }
    
    if (commandNames.length() == 0) {
        sendErrorResponse(400, "Missing 'commandNames' field");
        return;
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
        sendErrorResponse(400, errorMsg);
        return;
    }
    
    // 启动学习
    if (learner->startDeviceLearn(deviceName, commandNames, timeoutSeconds)) {
        DynamicJsonDocument response(256);
        response["success"] = true;
        response["message"] = "学习已启动";
        String output;
        output.reserve(256);
        serializeJson(response, output);
        sendJSONResponse(200, output);
    } else {
        sendErrorResponse(500, "Failed to start learning (可能已有学习任务进行中)");
    }
}

void WebServer::handleAPILearnStatus() {
    if (learner == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 内存保护：检查可用内存（提高阈值，因为需要创建JSON和String对象）
    if (ESP.getFreeHeap() < 3072) {
        sendErrorResponse(503, "Low Memory");
        return;
    }
    
    bool isLearning = learner->isLearningDevice();
    
    // 根据是否在学习中动态调整JSON大小（增加大小以避免溢出）
    size_t jsonSize = isLearning ? 1024 : 256;
    
    // 再次检查内存，确保有足够空间
    if (ESP.getFreeHeap() < jsonSize + 1024) {
        sendErrorResponse(503, "Low Memory");
        return;
    }
    
    DynamicJsonDocument doc(jsonSize);
    doc["isLearning"] = isLearning;
    
    if (isLearning) {
        // 直接使用String对象，但立即添加到JSON中，避免同时持有多个String对象
        String deviceName = learner->getDeviceName();
        doc["deviceName"] = deviceName;
        deviceName = "";  // 立即释放
        
        String progress = learner->getLearnProgress();
        doc["progress"] = progress;
        progress = "";  // 立即释放
        
        String currentCommand = learner->getCurrentCommandName();
        doc["currentCommand"] = currentCommand;
        currentCommand = "";  // 立即释放
        
        doc["learnedCount"] = learner->getLearnedCommandCount();
        doc["totalCount"] = learner->getTotalCommandCount();
        
        // 使用const char*指针数组，避免创建String对象，节省内存
        const char* learnedNames[MAX_LEARN_COMMANDS];
        learner->getLearnedCommandNames(learnedNames, MAX_LEARN_COMMANDS);
        JsonArray learnedCommandsArray = doc.createNestedArray("learnedCommands");
        int learnedCount = learner->getLearnedCommandCount();
        for (int i = 0; i < learnedCount && i < MAX_LEARN_COMMANDS; i++) {
            if (learnedNames[i] != nullptr && strlen(learnedNames[i]) > 0) {
                learnedCommandsArray.add(learnedNames[i]);
            }
        }
    } else {
        doc["deviceName"] = "";
        doc["progress"] = "";
        doc["currentCommand"] = "";
        doc["learnedCount"] = 0;
        doc["totalCount"] = 0;
        doc.createNestedArray("learnedCommands");  // 创建空数组以保持JSON结构一致
    }
    
    String output;
    output.reserve(jsonSize);
    serializeJson(doc, output);
    sendJSONResponse(200, output);
}

void WebServer::handleAPILearnStop() {
    if (learner == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    if (learner->stopDeviceLearn()) {
        DynamicJsonDocument response(192);
        response["success"] = true;
        response["message"] = "Device learn stopped";
        String output;
        output.reserve(192);
        serializeJson(response, output);
        sendJSONResponse(200, output);
    } else {
        sendErrorResponse(500, "Device learn is not active");
    }
}

void WebServer::handleAPILearnResult() {
    if (learner == nullptr) {
        Serial.println("[handleAPILearnResult] 错误：learner为null");
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 首先检查内存，确保有足够空间
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 6144) {  // 至少需要6KB空闲内存
        Serial.printf("[handleAPILearnResult] 错误：内存严重不足（需要: 6144, 可用: %d）\n", freeHeap);
        sendErrorResponse(503, "Low Memory");
        return;
    }
    
    // 不要复制整个LearnedDevice结构体到栈上（约5KB，会导致栈溢出）
    // 直接检查学习状态，然后调用convertToSmartIRFormat
    int commandCount = learner->getLearnedCommandCount();
    
    if (commandCount == 0) {
        Serial.println("[handleAPILearnResult] 错误：未找到学习设备（命令数为0）");
        sendErrorResponse(404, "No learned device found");
        return;
    }
    
    // 转换为SmartIR格式
    String smartIRJson;
    {
        // 使用作用域确保临时对象及时释放
        smartIRJson = learner->convertToSmartIRFormat();
    }
    
    // 检查转换是否成功（如果内存不足，可能返回空字符串或"{}"）
    if (smartIRJson.length() == 0 || smartIRJson == "{}") {
        Serial.println("[handleAPILearnResult] 错误：转换失败或返回空JSON");
        sendErrorResponse(503, "Failed to convert to SmartIR format (memory insufficient)");
        return;
    }
    
    // 检查内存是否仍然足够发送响应
    size_t freeHeapAfter = ESP.getFreeHeap();
    if (freeHeapAfter < smartIRJson.length() + 2048) {
        Serial.println("[handleAPILearnResult] 错误：内存不足，无法发送响应");
        sendErrorResponse(503, "Low Memory");
        return;
    }
    sendJSONResponse(200, smartIRJson);
    Serial.println("[handleAPILearnResult] 响应发送完成");
}

void WebServer::handleAPIRecentSignals() {
    if (logger == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 获取最近接收的信号
    RecentSignal signals[MAX_RECENT_SIGNALS];
    int count = logger->getRecentSignals(signals, MAX_RECENT_SIGNALS);
    
    // 简化：根据信号数量计算JSON大小
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
        signal["protocol"] = signals[i].signal.protocol;
        signal["data"] = signals[i].signal.data;
        signal["hash"] = signals[i].signal.hash;
        signal["isProtocol"] = signals[i].signal.isProtocol;
    }
    
    String output;
    output.reserve(jsonSize);
    serializeJson(doc, output);
    sendJSONResponse(200, output);
}

void WebServer::handleAPISendSignal() {
    if (forwarder == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    String body = server.arg("plain");
    if (body.length() == 0) {
        sendErrorResponse(400, "Missing request body");
        return;
    }
    
    // 简化：根据body长度动态计算JSON大小
    size_t jsonSize = body.length() + 256;
    if (jsonSize > 1024) jsonSize = 1024;
    if (jsonSize < 256) jsonSize = 256;
    
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, body);
    
    if (error) {
        // 优化：避免创建临时String对象
        char errorMsg[64];
        snprintf(errorMsg, sizeof(errorMsg), "Invalid JSON: %s", error.c_str());
        sendErrorResponse(400, errorMsg);
        return;
    }
    
    JsonObject signalObj = doc["signal"];
    if (signalObj.isNull()) {
        sendErrorResponse(400, "Missing 'signal' field");
        return;
    }
    
    SignalID signal;
    signal.protocol = signalObj["protocol"] | 0;
    signal.data = signalObj["data"] | 0;
    signal.hash = signalObj["hash"] | 0;
    signal.isProtocol = signalObj["isProtocol"] | false;
    
    uint8_t repeatCount = doc["repeatCount"] | 1;
    
    // 直接发送信号（不经过转发规则）
    if (forwarder->sendSignal(signal, repeatCount)) {
        DynamicJsonDocument response(192);
        response["success"] = true;
        response["message"] = "Signal sent";
        String output;
        output.reserve(192);
        serializeJson(response, output);
        sendJSONResponse(200, output);
    } else {
        sendErrorResponse(500, "Failed to send signal");
    }
}

