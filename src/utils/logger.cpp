#include "logger.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

Logger::Logger() : log_index(0), log_count(0), needsSave(false),
                   recentSignalIndex(0), recentSignalCount(0) {
    for (int i = 0; i < MAX_LOG_ENTRIES; i++) {
        logs[i].valid = false;
        logs[i].type[0] = '\0';
        logs[i].content[0] = '\0';
    }
    for (int i = 0; i < MAX_RECENT_SIGNALS; i++) {
        recentSignals[i].valid = false;
    }
}

bool Logger::init() {
    if (!LittleFS.begin()) {
        return false;
    }
    loadFromFlash();
    return true;
}

void Logger::log(const String& type, const String& content) {
    unsigned long now = millis();
    
    // 循环覆盖
    logs[log_index].timestamp = now;
    // 使用strncpy安全复制，确保以null结尾
    const char* typeStr = type.c_str();
    const char* contentStr = content.c_str();
    if (typeStr != nullptr) {
        strncpy(logs[log_index].type, typeStr, MAX_LOG_TYPE_LEN - 1);
        logs[log_index].type[MAX_LOG_TYPE_LEN - 1] = '\0';
    } else {
        logs[log_index].type[0] = '\0';
    }
    if (contentStr != nullptr) {
        strncpy(logs[log_index].content, contentStr, MAX_LOG_CONTENT_LEN - 1);
        logs[log_index].content[MAX_LOG_CONTENT_LEN - 1] = '\0';
    } else {
        logs[log_index].content[0] = '\0';
    }
    logs[log_index].valid = true;
    
    log_index = (log_index + 1) % MAX_LOG_ENTRIES;
    if (log_count < MAX_LOG_ENTRIES) {
        log_count++;
    }
    
    // 串口输出
    Serial.printf("[%lu] [%s] %s\n", now, logs[log_index == 0 ? MAX_LOG_ENTRIES - 1 : log_index - 1].type, 
                  logs[log_index == 0 ? MAX_LOG_ENTRIES - 1 : log_index - 1].content);
    
    // 优化：只在达到阈值或内存压力大时才标记保存
    // 如果日志数量达到一定阈值（比如20条）或内存使用率较高，才标记保存
    // 这样可以减少Flash写入次数，延长Flash寿命
    static int logSaveThreshold = 20;  // 日志数量阈值
    if (log_count >= logSaveThreshold) {
        needsSave = true;
    }
    // 注意：错误日志总是立即标记保存（重要信息）
    if (type == "error") {
        needsSave = true;
    }
}

void Logger::logReceive(const String& signal_info) {
    log("receive", signal_info);
}

void Logger::logTransmit(const String& signal_info) {
    log("transmit", signal_info);
}

void Logger::logError(const String& error_msg) {
    log("error", error_msg);
}

void Logger::logLearn(const String& signal_info) {
    log("learn", signal_info);
}

void Logger::logReceiveSignal(const SignalID& signalId, bool matched) {
    unsigned long now = millis();
    
    // 记录到最近信号缓冲区（环形缓冲区）
    recentSignals[recentSignalIndex].signal = signalId;
    recentSignals[recentSignalIndex].timestamp = now;
    recentSignals[recentSignalIndex].matched = matched;
    recentSignals[recentSignalIndex].valid = true;
    
    recentSignalIndex = (recentSignalIndex + 1) % MAX_RECENT_SIGNALS;
    if (recentSignalCount < MAX_RECENT_SIGNALS) {
        recentSignalCount++;
    }
    
    // 同时记录到日志（使用字符串格式）
    String signalStr = "Protocol:" + String(signalId.protocol) + 
                      ", Data:" + String(signalId.data, HEX) +
                      ", Hash:" + String(signalId.hash, HEX) +
                      ", Matched:" + String(matched ? "Yes" : "No");
    logReceive(signalStr);
}

int Logger::getRecentSignals(RecentSignal* signals, int max_count) {
    if (signals == nullptr || max_count <= 0) {
        return 0;
    }
    
    int copy_count = (max_count < recentSignalCount) ? max_count : recentSignalCount;
    int start_idx = (recentSignalCount < MAX_RECENT_SIGNALS) ? 0 : recentSignalIndex;
    
    for (int i = 0; i < copy_count; i++) {
        int idx = (start_idx + i) % MAX_RECENT_SIGNALS;
        if (recentSignals[idx].valid) {
            signals[i] = recentSignals[idx];
        }
    }
    
    return copy_count;
}

int Logger::getLogCount() const {
    return log_count;
}

bool Logger::getLogs(LogEntry* entries, int max_count) {
    if (entries == nullptr || max_count <= 0) {
        return false;
    }
    
    int copy_count = (max_count < log_count) ? max_count : log_count;
    int start_idx = (log_count < MAX_LOG_ENTRIES) ? 0 : log_index;
    
    for (int i = 0; i < copy_count; i++) {
        int idx = (start_idx + i) % MAX_LOG_ENTRIES;
        if (logs[idx].valid) {
            entries[i] = logs[idx];
        }
    }
    
    return true;
}

void Logger::clearLogs() {
    for (int i = 0; i < MAX_LOG_ENTRIES; i++) {
        logs[i].valid = false;
        logs[i].type[0] = '\0';
        logs[i].content[0] = '\0';
    }
    log_index = 0;
    log_count = 0;
    needsSave = true;
    saveToFlash();
    needsSave = false;
}

void Logger::publishToHA(const String& type, const String& content) {
    // 此函数由HA接口模块调用，这里只记录
    // 实际推送在 ha_mqtt_handler 中实现
}

void Logger::flush() {
    if (needsSave) {
        saveToFlash();
    }
}

void Logger::saveToFlash() {
    // 延迟保存：只在需要时保存
    if (!needsSave) {
        return;
    }
    
    File file = LittleFS.open(LOG_FILE, "w");
    if (!file) {
        return;
    }
    
    // 动态计算JSON大小：每条日志约100字节，加上JSON开销
    // 30条日志约3000字节，加上数组开销约4000字节
    size_t jsonSize = (log_count * 100) + 1024;
    if (jsonSize < 2048) jsonSize = 2048;  // 最小2KB
    DynamicJsonDocument doc(jsonSize);
    JsonArray logArray = doc.to<JsonArray>();
    
    int start_idx = (log_count < MAX_LOG_ENTRIES) ? 0 : log_index;
    for (int i = 0; i < log_count; i++) {
        int idx = (start_idx + i) % MAX_LOG_ENTRIES;
        if (logs[idx].valid) {
            JsonObject entry = logArray.createNestedObject();
            entry["timestamp"] = logs[idx].timestamp;
            entry["type"] = logs[idx].type;
            entry["content"] = logs[idx].content;
        }
    }
    
    serializeJson(doc, file);
    file.close();
    needsSave = false;
}

void Logger::loadFromFlash() {
    if (!LittleFS.exists(LOG_FILE)) {
        return;
    }
    
    File file = LittleFS.open(LOG_FILE, "r");
    if (!file) {
        return;
    }
    
    // 动态计算JSON大小
    size_t fileSize = file.size();
    size_t jsonSize = fileSize + 512;  // 加上解析开销
    if (jsonSize < 2048) jsonSize = 2048;
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        return;
    }
    
    JsonArray logArray = doc.as<JsonArray>();
    log_count = 0;
    
    for (JsonObject entry : logArray) {
        if (log_count >= MAX_LOG_ENTRIES) {
            break;
        }
        
        logs[log_count].timestamp = entry["timestamp"];
        const char* typeStr = entry["type"] | "";
        const char* contentStr = entry["content"] | "";
        strncpy(logs[log_count].type, typeStr, MAX_LOG_TYPE_LEN - 1);
        logs[log_count].type[MAX_LOG_TYPE_LEN - 1] = '\0';
        strncpy(logs[log_count].content, contentStr, MAX_LOG_CONTENT_LEN - 1);
        logs[log_count].content[MAX_LOG_CONTENT_LEN - 1] = '\0';
        logs[log_count].valid = true;
        log_count++;
    }
    
    log_index = log_count % MAX_LOG_ENTRIES;
    needsSave = false;
}

