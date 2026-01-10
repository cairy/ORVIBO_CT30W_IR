#include "web_server.h"
#include "ir_core/ir_mapper.h"
#include "ir_core/ir_forwarder.h"
#include "ir_core/ir_learner.h"
#include "utils/logger.h"
#include "utils/memory_monitor.h"
#include "utils/hex_utils.h"
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
    
    // 优化：预分配更小的固定大小，减少内存占用
    String html;
    html.reserve(800);  // 从1500降至800，节省700字节
    
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

// 内存检查辅助函数：在API处理前检查内存是否充足
// requiredBytes: 所需的最小连续内存块大小（字节）
// 返回true表示内存充足，false表示内存不足
bool WebServer::checkMemoryBeforeAPI(size_t requiredBytes) {
    size_t freeHeap = ESP.getFreeHeap();
    size_t maxBlock = ESP.getMaxFreeBlockSize();
    float usagePercent = MemoryMonitor::getUsagePercent();
    
    // 优先检查实际可用内存，而不是百分比
    // 如果空闲内存足够（大于2KB）且最大连续块足够，即使使用率高也允许请求
    if (freeHeap >= 2048 && maxBlock >= requiredBytes + 512) {
        return true;
    }
    
    // 如果空闲内存不足，再检查百分比（提高到93%）
    if (usagePercent > 93.0f) {
        Serial.printf("[Web服务器] 内存使用率过高 (%.1f%%), 拒绝API请求\n", usagePercent);
        return false;
    }
    
    // 空闲内存小于512字节时拒绝请求
    if (freeHeap < 512) {
        Serial.printf("[Web服务器] 空闲内存不足 (%d bytes), 拒绝API请求\n", freeHeap);
        return false;
    }
    
    // 最大连续块小于所需大小+256字节安全余量时拒绝请求
    if (maxBlock < requiredBytes + 256) {
        Serial.printf("[Web服务器] 最大连续块不足 (%d bytes < %d bytes), 拒绝API请求\n", 
                     maxBlock, requiredBytes + 256);
        return false;
    }
    
    return true;
}

void WebServer::handleAPIStatus() {
    // 简单API，使用较小的内存检查阈值
    if (!checkMemoryBeforeAPI(256)) {
        sendErrorResponse(503, "Insufficient memory");
        return;
    }
    
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
    // 内存检查：需要较大内存用于规则数组和JSON
    if (!checkMemoryBeforeAPI(3000)) {
        sendErrorResponse(503, "Insufficient memory");
        return;
    }
    
    if (mapper == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    int count = mapper->getRuleCount();
    
    // 限制最大数量到10个，避免栈溢出和内存不足
    int maxRules = (count < 10) ? count : 10;
    
    // 使用堆分配而不是栈分配，避免栈溢出
    IR_MapRule* rules = new (std::nothrow) IR_MapRule[maxRules];
    if (rules == nullptr) {
        sendErrorResponse(503, "Failed to allocate memory for rules");
        return;
    }
    
    bool success = mapper->getAllRules(rules, maxRules);
    if (!success) {
        delete[] rules;
        sendErrorResponse(500, "Failed to get rules");
        return;
    }
    
    // 优化JSON大小估算：基础开销 + 每条规则约200字节，限制最大4KB
    size_t jsonSize = 256 + (maxRules * 200);
    if (jsonSize > 4096) jsonSize = 4096;
    if (jsonSize < 512) jsonSize = 512;
    
    DynamicJsonDocument doc(jsonSize);
    JsonArray rulesArray = doc.to<JsonArray>();
    
    for (int i = 0; i < maxRules; i++) {
        JsonObject ruleObj = rulesArray.createNestedObject();
        ruleObj["id"] = rules[i].id;
        
        JsonObject aSignal = ruleObj.createNestedObject("aSignal");
        aSignal["protocol"] = rules[i].aSignal.protocol;
        aSignal["data"] = uint32ToHexString(rules[i].aSignal.data);
        aSignal["hash"] = uint32ToHexString(rules[i].aSignal.hash);
        aSignal["isProtocol"] = rules[i].aSignal.isProtocol;
        
        JsonArray bSignals = ruleObj.createNestedArray("bSignals");
        for (int j = 0; j < rules[i].bSignalCount; j++) {
            JsonObject bSignal = bSignals.createNestedObject();
            bSignal["protocol"] = rules[i].bSignals[j].protocol;
            bSignal["data"] = uint32ToHexString(rules[i].bSignals[j].data);
            bSignal["hash"] = uint32ToHexString(rules[i].bSignals[j].hash);
            bSignal["isProtocol"] = rules[i].bSignals[j].isProtocol;
        }
        
        ruleObj["bSignalCount"] = rules[i].bSignalCount;
        
        JsonArray intervals = ruleObj.createNestedArray("intervals");
        for (int j = 0; j < rules[i].bSignalCount - 1; j++) {
            intervals.add(rules[i].intervals[j]);
        }
        
        ruleObj["repeatCount"] = rules[i].repeatCount;
    }
    
    // 立即释放规则数组内存
    delete[] rules;
    
    String output;
    output.reserve(jsonSize);
    serializeJson(doc, output);
    sendJSONResponse(200, output);
}

void WebServer::handleAPIUpdateMap() {
    // 内存检查：需要解析JSON和更新规则
    String body = server.arg("plain");
    if (body.length() == 0) {
        sendErrorResponse(400, "Missing request body");
        return;
    }
    
    // 根据body长度动态计算JSON大小，限制最大4KB
    size_t jsonSize = body.length() + 512;
    if (jsonSize > 4096) jsonSize = 4096;
    if (jsonSize < 512) jsonSize = 512;
    
    // 检查内存是否足够
    if (!checkMemoryBeforeAPI(jsonSize + 1024)) {
        sendErrorResponse(503, "Insufficient memory");
        return;
    }
    
    if (mapper == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
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
        uint8_t ruleIds[10];  // 限制最多10个规则ID（每个ID 1字节，总共10字节）
        int idCount = 0;
        
        // 获取所有规则ID（使用较小的临时数组，避免栈溢出）
        int count = mapper->getRuleCount();
        if (count > 10) count = 10;  // 限制最多10个规则
        
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
            if (aSignal["data"].is<const char*>()) {
                rule.aSignal.data = parseUint32FromHexString(aSignal["data"].as<const char*>());
            } else {
                rule.aSignal.data = 0;
            }
            if (aSignal["hash"].is<const char*>()) {
                rule.aSignal.hash = parseUint32FromHexString(aSignal["hash"].as<const char*>());
            } else {
                rule.aSignal.hash = 0;
            }
            rule.aSignal.isProtocol = aSignal["isProtocol"] | false;
            
            JsonArray bSignals = ruleObj["bSignals"];
            rule.bSignalCount = 0;
            for (JsonObject bSignal : bSignals) {
                if (rule.bSignalCount >= MAX_B_SIGNALS_PER_RULE) break;
                
                rule.bSignals[rule.bSignalCount].protocol = bSignal["protocol"] | 0;
                if (bSignal["data"].is<const char*>()) {
                    rule.bSignals[rule.bSignalCount].data = parseUint32FromHexString(bSignal["data"].as<const char*>());
                } else {
                    rule.bSignals[rule.bSignalCount].data = 0;
                }
                if (bSignal["hash"].is<const char*>()) {
                    rule.bSignals[rule.bSignalCount].hash = parseUint32FromHexString(bSignal["hash"].as<const char*>());
                } else {
                    rule.bSignals[rule.bSignalCount].hash = 0;
                }
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
        if (aSignal["data"].is<const char*>()) {
            rule.aSignal.data = parseUint32FromHexString(aSignal["data"].as<const char*>());
        } else {
            rule.aSignal.data = 0;
        }
        if (aSignal["hash"].is<const char*>()) {
            rule.aSignal.hash = parseUint32FromHexString(aSignal["hash"].as<const char*>());
        } else {
            rule.aSignal.hash = 0;
        }
        rule.aSignal.isProtocol = aSignal["isProtocol"] | false;
        
        JsonArray bSignals = ruleObj["bSignals"];
        rule.bSignalCount = 0;
        for (JsonObject bSignal : bSignals) {
            if (rule.bSignalCount >= MAX_B_SIGNALS_PER_RULE) break;
            
            rule.bSignals[rule.bSignalCount].protocol = bSignal["protocol"] | 0;
            if (bSignal["data"].is<const char*>()) {
                rule.bSignals[rule.bSignalCount].data = parseUint32FromHexString(bSignal["data"].as<const char*>());
            } else {
                rule.bSignals[rule.bSignalCount].data = 0;
            }
            if (bSignal["hash"].is<const char*>()) {
                rule.bSignals[rule.bSignalCount].hash = parseUint32FromHexString(bSignal["hash"].as<const char*>());
            } else {
                rule.bSignals[rule.bSignalCount].hash = 0;
            }
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
    // 内存检查
    if (!checkMemoryBeforeAPI(2048)) {
        sendErrorResponse(503, "Insufficient memory");
        return;
    }
    
    if (logger == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    LogEntry entries[MAX_LOG_ENTRIES];
    int count = logger->getLogCount();
    logger->getLogs(entries, count);
    
    // 优化：根据日志数量计算JSON大小，限制最大3KB
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
    String body = server.arg("plain");
    if (body.length() == 0) {
        sendErrorResponse(400, "Missing request body");
        return;
    }
    
    // 估算所需内存：JSON解析 + String对象 + 学习数据结构初始化
    size_t jsonSize = body.length() + 256;
    if (jsonSize > 1024) jsonSize = 1024;
    if (jsonSize < 256) jsonSize = 256;
    
    // 内存检查：学习功能需要较多内存
    if (!checkMemoryBeforeAPI(jsonSize + 2048)) {
        sendErrorResponse(503, "Insufficient memory");
        return;
    }
    
    if (learner == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
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
    // 简单API，使用较小的内存检查阈值
    if (!checkMemoryBeforeAPI(256)) {
        sendErrorResponse(503, "Insufficient memory");
        return;
    }
    
    if (learner == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    bool isLearning = learner->isLearningDevice();
    
    // 使用固定大小的JSON文档（512字节足够）
    DynamicJsonDocument doc(512);
    doc["isLearning"] = isLearning;
    
    if (isLearning) {
        String progress = learner->getLearnProgress();
        doc["progress"] = progress;
        
        String currentCommand = learner->getCurrentCommandName();
        doc["currentCommand"] = currentCommand;
    } else {
        doc["progress"] = "";
        doc["currentCommand"] = "";
    }
    
    String output;
    serializeJson(doc, output);
    sendJSONResponse(200, output);
}

void WebServer::handleAPILearnStop() {
    // 简单API，使用较小的内存检查阈值
    if (!checkMemoryBeforeAPI(256)) {
        sendErrorResponse(503, "Insufficient memory");
        return;
    }
    
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
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    int commandCount = learner->getLearnedCommandCount();
    if (commandCount == 0) {
        sendErrorResponse(404, "No learned device found");
        return;
    }
    
    // 内存检查：转换结果可能很大（最多3KB），需要更多内存
    if (!checkMemoryBeforeAPI(4096)) {
        sendErrorResponse(503, "Insufficient memory");
        return;
    }
    
    // 获取格式参数（默认base64）
    String format = server.arg("format");
    if (format.length() == 0) {
        format = "base64";  // 默认格式
    }
    format.toLowerCase();  // 转换为小写，使比较大小写不敏感
    
    String resultJson;
    if (format == "signalid") {
        resultJson = learner->convertToSignalIDFormat();
    } else if (format == "array") {
        resultJson = learner->convertToArrayFormat();
    } else {
        // 默认使用base64格式
        resultJson = learner->convertToBase64Format();
    }
    
    if (resultJson.length() == 0 || resultJson == "{}") {
        sendErrorResponse(503, "Failed to convert format");
        return;
    }
    
    // 立即发送响应，然后释放String对象
    sendJSONResponse(200, resultJson);
    // String对象会在函数返回时自动释放
}

void WebServer::handleAPIRecentSignals() {
    // 内存检查
    if (!checkMemoryBeforeAPI(1536)) {
        sendErrorResponse(503, "Insufficient memory");
        return;
    }
    
    if (logger == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 获取最近接收的信号
    RecentSignal signals[MAX_RECENT_SIGNALS];
    int count = logger->getRecentSignals(signals, MAX_RECENT_SIGNALS);
    
    // 优化：根据信号数量计算JSON大小，限制最大1.5KB
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
        signal["data"] = uint32ToHexString(signals[i].signal.data);
        signal["hash"] = uint32ToHexString(signals[i].signal.hash);
        signal["isProtocol"] = signals[i].signal.isProtocol;
    }
    
    String output;
    output.reserve(jsonSize);
    serializeJson(doc, output);
    sendJSONResponse(200, output);
}

void WebServer::handleAPISendSignal() {
    String body = server.arg("plain");
    if (body.length() == 0) {
        sendErrorResponse(400, "Missing request body");
        return;
    }
    
    String trimmed = body;
    trimmed.trim();
    
    if (trimmed.length() == 0) {
        sendErrorResponse(400, "Empty request body");
        return;
    }
    
    // 检测格式
    IRLearner::SignalFormat format = IRLearner::detectSignalFormat(trimmed);
    
    // 如果检测到JSON格式（FORMAT_SIGNALID），按JSON格式解析
    if (format == IRLearner::FORMAT_SIGNALID) {
        // SignalID对象格式（JSON）
        size_t jsonSize = trimmed.length() + 256;
        if (jsonSize > 1024) jsonSize = 1024;
        if (jsonSize < 256) jsonSize = 256;
        
        // 内存检查
        if (!checkMemoryBeforeAPI(jsonSize + 512)) {
            sendErrorResponse(503, "Insufficient memory");
            return;
        }
        
        if (forwarder == nullptr) {
            sendErrorResponse(500, "Service not initialized");
            return;
        }
        
        DynamicJsonDocument doc(jsonSize);
        DeserializationError error = deserializeJson(doc, trimmed);
        
        if (error) {
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
        
        uint8_t repeatCount = doc["repeatCount"] | 1;
        
        SignalID signal;
        signal.protocol = signalObj["protocol"] | 0;
        if (signalObj["data"].is<const char*>()) {
            signal.data = parseUint32FromHexString(signalObj["data"].as<const char*>());
        } else {
            signal.data = 0;
        }
        if (signalObj["hash"].is<const char*>()) {
            signal.hash = parseUint32FromHexString(signalObj["hash"].as<const char*>());
        } else {
            signal.hash = 0;
        }
        signal.isProtocol = signalObj["isProtocol"] | false;
        
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
    } else {
        // 内存检查：解析信号可能需要较多内存
        if (!checkMemoryBeforeAPI(2048)) {
            sendErrorResponse(503, "Insufficient memory");
            return;
        }
        
        if (forwarder == nullptr) {
            sendErrorResponse(500, "Service not initialized");
            return;
        }
        
        // 字符串格式（Base64/数组/协议），使用统一解析函数
        SignalID signalId;
        static IRTiming timing[MAX_LEARN_TIMING_COUNT];
        int timingCount = 0;
        uint8_t repeatCount = 1;
        
        if (!IRLearner::parseSignalPayload(trimmed, signalId, timing, timingCount, MAX_LEARN_TIMING_COUNT, repeatCount, format)) {
            sendErrorResponse(400, "Failed to parse signal");
            return;
        }
        
        // 根据格式发送信号
        bool success = false;
        if (format == IRLearner::FORMAT_PROTOCOL) {
            success = forwarder->sendSignal(signalId, repeatCount);
        } else {
            success = forwarder->sendRawTiming(timing, timingCount, repeatCount);
        }
        
        if (success) {
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
}

