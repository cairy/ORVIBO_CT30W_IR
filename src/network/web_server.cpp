#include "web_server.h"
#include "ir_core/ir_mapper.h"
#include "ir_core/ir_forwarder.h"
#include "ir_core/ir_learner.h"
#include "utils/logger.h"
#include "utils/memory_monitor.h"
#include "utils/hex_utils.h"
#include "utils/api_response.h"
#include "utils/api_handlers.h"
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
    return MemoryMonitor::checkMemory(requiredBytes);
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
    
    // 使用统一格式
    DynamicJsonDocument doc(256);
    JsonObject dataObj = doc.createNestedObject("data");
    dataObj["status"] = "running";
    dataObj["ruleCount"] = mapper->getRuleCount();
    dataObj["forwarding"] = forwarder->isForwarding();
    
    String output = APIResponse::success(dataObj, logger);
    sendJSONResponse(200, output);
}

void WebServer::handleAPIMaps() {
    if (mapper == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 使用公共函数，传递 logger
    String dataJson = APIHandlers::handleQueryMaps(*mapper, logger);
    if (dataJson.length() == 0) {
        sendErrorResponse(503, "Insufficient memory");
        return;
    }
    
    // 解析为JsonArray并包装为统一响应
    // 动态计算JSON大小：解析JSON需要的内存通常是字符串长度的1.5-2倍
    // 加上响应包装开销（success, status, timestamp, data字段）
    size_t docSize = (dataJson.length() * 2) + 1024;  // 2倍 + 1KB包装开销
    if (docSize < 2048) docSize = 2048;  // 最小2KB
    if (docSize > 16384) docSize = 16384;  // 限制最大16KB，避免内存不足
    
    DynamicJsonDocument dataDoc(docSize);
    DeserializationError err = deserializeJson(dataDoc, dataJson);
    if (err) {
        char errorMsg[128];
        snprintf(errorMsg, sizeof(errorMsg), "Failed to parse rules: %s", err.c_str());
        if (logger != nullptr) {
            logger->log("error", errorMsg);
        }
        sendErrorResponse(500, errorMsg);
        return;
    }
    
    // 检查文档是否溢出
    if (dataDoc.overflowed()) {
        char buffer[192];
        snprintf(buffer, sizeof(buffer), "[接口: /api/maps] JSON文档溢出，尝试使用更大的文档 (当前大小: %d bytes, 数据长度: %d)", docSize, dataJson.length());
        if (logger != nullptr) {
            logger->log("warning", buffer);
        }
        
        // 尝试使用更大的文档重新解析
        size_t largerSize = dataJson.length() * 2 + 2048;
        if (largerSize <= 16384) {  // 限制最大16KB
            DynamicJsonDocument largerDoc(largerSize);
            DeserializationError largerErr = deserializeJson(largerDoc, dataJson);
            if (!largerErr && !largerDoc.overflowed()) {
                dataDoc = largerDoc;
            } else {
                if (logger != nullptr) {
                    logger->log("error", "[接口: /api/maps] 更大的文档仍然溢出或解析失败");
                }
                sendErrorResponse(500, "JSON document overflowed");
                return;
            }
        } else {
            if (logger != nullptr) {
                logger->log("error", "[接口: /api/maps] JSON文档过大，无法重试");
            }
            sendErrorResponse(500, "JSON too large");
            return;
        }
    }
    
    JsonArray rulesArray = dataDoc.as<JsonArray>();
    String output = APIResponse::success(rulesArray, logger, "/api/maps");
    
    sendJSONResponse(200, output);
}

void WebServer::handleAPIUpdateMap() {
    String body = server.arg("plain");
    if (body.length() == 0) {
        sendErrorResponse(400, "Missing request body");
        return;
    }
    
    if (mapper == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 使用公共函数
    bool success = false;
    String message = APIHandlers::handleUpdateMap(*mapper, body, success);
    
    if (success) {
        sendJSONResponse(200, APIResponse::success(message, logger));
    } else {
        sendErrorResponse(400, message);
    }
}

void WebServer::handleAPILogs() {
    if (logger == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 使用公共函数（返回所有日志，系统只保留3条）
    String dataJson = APIHandlers::handleGetLogs(*logger);
    if (dataJson.length() == 0) {
        sendErrorResponse(503, "Insufficient memory");
        return;
    }
    
    // 解析为JsonArray并包装为统一响应
    DynamicJsonDocument dataDoc(1024);
    DeserializationError err = deserializeJson(dataDoc, dataJson);
    if (err) {
        sendErrorResponse(500, "Failed to parse logs");
        return;
    }
    
    JsonArray logArray = dataDoc.as<JsonArray>();
    String output = APIResponse::success(logArray, logger);
    sendJSONResponse(200, output);
}

void WebServer::handleAPILearnStart() {
    String body = server.arg("plain");
    if (body.length() == 0) {
        sendErrorResponse(400, "Missing request body");
        return;
    }
    
    if (learner == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 使用公共函数
    bool success = false;
    String message = APIHandlers::handleStartLearn(*learner, body, success);
    
    if (success) {
        sendJSONResponse(200, APIResponse::success(message, logger));
    } else {
        sendErrorResponse(400, message);
    }
}

void WebServer::handleAPILearnStatus() {
    if (learner == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 使用公共函数（简化版）
    String dataJson = APIHandlers::handleQueryLearnStatus(*learner);
    if (dataJson.length() == 0) {
        sendErrorResponse(503, "Insufficient memory");
        return;
    }
    
    // 解析为JsonObject并包装为统一响应
    DynamicJsonDocument dataDoc(512);
    DeserializationError err = deserializeJson(dataDoc, dataJson);
    if (err) {
        sendErrorResponse(500, "Failed to parse learn status");
        return;
    }
    
    JsonObject dataObj = dataDoc.as<JsonObject>();
    String output = APIResponse::success(dataObj, logger);
    sendJSONResponse(200, output);
}

void WebServer::handleAPILearnStop() {
    if (learner == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 使用公共函数
    bool success = false;
    String message = APIHandlers::handleStopLearn(*learner, success);
    
    if (success) {
        sendJSONResponse(200, APIResponse::success(message, logger));
    } else {
        sendErrorResponse(500, message);
    }
}

void WebServer::handleAPILearnResult() {
    if (learner == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 获取格式参数（默认base64）
    String format = server.arg("format");
    if (format.length() == 0) {
        format = "base64";  // 默认格式
    }
    
    // 使用公共函数
    String dataJson = APIHandlers::handleQueryLearnResult(*learner, format);
    if (dataJson.length() == 0) {
        sendErrorResponse(404, "No learned device found");
        return;
    }
    
    // 尝试解析为JSON对象或数组，如果失败则作为字符串返回
    DynamicJsonDocument dataDoc(4096);
    DeserializationError err = deserializeJson(dataDoc, dataJson);
    
    if (err) {
        // 如果不是JSON，直接作为字符串返回
        String output = APIResponse::success(dataJson, logger);
        sendJSONResponse(200, output);
    } else {
        // 如果是JSON，提取为JsonObject或JsonArray
        if (dataDoc.is<JsonObject>()) {
            JsonObject obj = dataDoc.as<JsonObject>();
            String output = APIResponse::success(obj, logger);
            sendJSONResponse(200, output);
        } else if (dataDoc.is<JsonArray>()) {
            JsonArray arr = dataDoc.as<JsonArray>();
            String output = APIResponse::success(arr, logger);
            sendJSONResponse(200, output);
        } else {
            // 其他情况，作为字符串返回
            String output = APIResponse::success(dataJson, logger);
            sendJSONResponse(200, output);
        }
    }
}

void WebServer::handleAPIRecentSignals() {
    if (logger == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 使用公共函数
    String dataJson = APIHandlers::handleGetRecentSignals(*logger);
    if (dataJson.length() == 0) {
        sendErrorResponse(503, "Insufficient memory");
        return;
    }
    
    // 解析为JsonArray并包装为统一响应
    DynamicJsonDocument dataDoc(1536);
    DeserializationError err = deserializeJson(dataDoc, dataJson);
    if (err) {
        sendErrorResponse(500, "Failed to parse signals");
        return;
    }
    
    JsonArray signalsArray = dataDoc.as<JsonArray>();
    String output = APIResponse::success(signalsArray, logger);
    sendJSONResponse(200, output);
}

void WebServer::handleAPISendSignal() {
    String body = server.arg("plain");
    if (body.length() == 0) {
        sendErrorResponse(400, "Missing request body");
        return;
    }
    
    if (forwarder == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 使用公共函数
    bool success = false;
    String message = APIHandlers::handleSendSignal(*forwarder, body, success);
    
    if (success) {
        sendJSONResponse(200, APIResponse::executing(message));
    } else {
        sendErrorResponse(500, message);
    }
}

