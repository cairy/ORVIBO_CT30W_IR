#include "web_server.h"
#include "ir_core/ir_mapper.h"
#include "ir_core/ir_forwarder.h"
#include "ir_core/ir_learner.h"
#include "utils/logger.h"
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
    
    if (mqtt_server.length() > 0) {
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
    
    // 预分配String容量以减少内存重新分配
    String html;
    html.reserve(1500);  // 预估HTML大小约1500字节
    
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
    html += F("<input type='password' name='password' placeholder='WiFi 密码' value='");
    html += config.wifi_password;
    html += F("' required>");
    html += F("<h2>MQTT 配置</h2>");
    html += F("<input type='text' name='mqtt_server' placeholder='MQTT 服务器地址' value='");
    html += config.mqtt_server;
    html += F("' required>");
    html += F("<input type='number' name='mqtt_port' placeholder='MQTT 端口' value='");
    html += config.mqtt_port;
    html += F("'>");
    html += F("<input type='text' name='mqtt_username' placeholder='MQTT 用户名（可选）' value='");
    html += config.mqtt_username;
    html += F("'>");
    html += F("<input type='password' name='mqtt_password' placeholder='MQTT 密码（可选）' value='");
    html += config.mqtt_password;
    html += F("'>");
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
    serializeJson(doc, output);
    sendJSONResponse(code, output);
}

void WebServer::handleAPIStatus() {
    if (mapper == nullptr || forwarder == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    DynamicJsonDocument doc(512);
    doc["success"] = true;
    doc["status"] = "running";
    doc["ruleCount"] = mapper->getRuleCount();
    doc["forwarding"] = forwarder->isForwarding();
    
    String output;
    serializeJson(doc, output);
    sendJSONResponse(200, output);
}

void WebServer::handleAPIMaps() {
    if (mapper == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    IR_MapRule rules[MAX_MAP_RULES];
    int count = mapper->getRuleCount();
    mapper->getAllRules(rules, count);
    
    // 动态计算JSON大小：每条规则约200字节
    size_t jsonSize = (count * 200) + 2048;
    if (jsonSize < 2048) jsonSize = 2048;
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
    
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, body);
    
    if (error) {
        sendErrorResponse(400, "Invalid JSON: " + String(error.c_str()));
        return;
    }
    
    // 检查是否是批量更新（包含rules数组）
    if (doc.containsKey("rules")) {
        // 批量更新：清空现有规则，添加新规则
        JsonArray rulesArray = doc["rules"];
        
        // 清空所有现有规则
        IR_MapRule rules[MAX_MAP_RULES];
        int count = mapper->getRuleCount();
        mapper->getAllRules(rules, count);
        for (int i = 0; i < count; i++) {
            mapper->deleteRule(rules[i].id);
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
        
        DynamicJsonDocument response(256);
        response["success"] = true;
        response["message"] = "Rules updated";
        response["count"] = successCount;
        String output;
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
    
    // 动态计算JSON大小：每条日志约100字节
    size_t jsonSize = (count * 100) + 1024;
    if (jsonSize < 1024) jsonSize = 1024;
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
    sendJSONResponse(200, output);
}

void WebServer::handleAPILearnStop() {
    if (learner == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    if (learner->stopDeviceLearn()) {
        DynamicJsonDocument response(256);
        response["success"] = true;
        response["message"] = "Device learn stopped";
        String output;
        serializeJson(response, output);
        sendJSONResponse(200, output);
    } else {
        sendErrorResponse(500, "Device learn is not active");
    }
}

void WebServer::handleAPILearnResult() {
    if (learner == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    LearnedDevice device;
    if (!learner->getLearnedDevice(device)) {
        sendErrorResponse(404, "No learned device found");
        return;
    }
    
    // 转换为SmartIR格式
    String smartIRJson = learner->convertToSmartIRFormat();
    sendJSONResponse(200, smartIRJson);
}

void WebServer::handleAPIRecentSignals() {
    if (logger == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 获取最近接收的信号
    RecentSignal signals[MAX_RECENT_SIGNALS];
    int count = logger->getRecentSignals(signals, MAX_RECENT_SIGNALS);
    
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
    
    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, body);
    
    if (error) {
        sendErrorResponse(400, "Invalid JSON: " + String(error.c_str()));
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
        DynamicJsonDocument response(256);
        response["success"] = true;
        response["message"] = "Signal sent";
        String output;
        serializeJson(response, output);
        sendJSONResponse(200, output);
    } else {
        sendErrorResponse(500, "Failed to send signal");
    }
}

