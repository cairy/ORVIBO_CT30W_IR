#include "unified_api.h"
#include "utils/logger.h"

String UnifiedAPI::createSuccessResponse(JsonVariant data, const String& status, Logger* logger, const char* context) {
    size_t estimatedSize = 256;  // 基础开销：success, status, timestamp等字段
    
    if (data.is<JsonObject>()) {
        estimatedSize += 512;
    } else if (data.is<JsonArray>()) {
        // 对于JsonArray，需要更精确地估算大小
        // 先序列化data数组来获取实际大小，然后乘以1.5-2倍作为JSON文档的内存需求
        // 因为JSON文档需要存储对象结构，而不仅仅是序列化后的字符串
        JsonArray arr = data.as<JsonArray>();
        String temp;
        serializeJson(arr, temp);
        // JSON文档内存需求通常是序列化字符串长度的1.5-2倍
        // 加上包装开销（success, status, timestamp, data字段）
        estimatedSize = (temp.length() * 2) + 1024;  // 2倍 + 1KB包装开销
        // 限制最大大小，避免内存不足
        if (estimatedSize > 16384) estimatedSize = 16384;
        if (estimatedSize < 2048) estimatedSize = 2048;  // 最小2KB
    } else if (data.is<const char*>()) {
        estimatedSize += strlen(data.as<const char*>());
    }
    
    DynamicJsonDocument doc(estimatedSize);
    doc["success"] = true;
    doc["status"] = status;
    doc["data"] = data;
    addTimestamp(doc);
    
    // 检查文档是否溢出
    if (doc.overflowed()) {
        char buffer[192];
        if (context != nullptr) {
            snprintf(buffer, sizeof(buffer), "[%s] JSON文档溢出，尝试使用更大的文档 (当前大小: %d bytes)", context, estimatedSize);
        } else {
            snprintf(buffer, sizeof(buffer), "[UnifiedAPI] JSON文档溢出，尝试使用更大的文档 (当前大小: %d bytes)", estimatedSize);
        }
        if (logger != nullptr) {
            logger->log("warning", buffer);
        }
        
        // 如果溢出，尝试使用更大的文档
        size_t largerSize = estimatedSize * 2;
        if (largerSize <= 16384) {
            DynamicJsonDocument largerDoc(largerSize);
            largerDoc["success"] = true;
            largerDoc["status"] = status;
            largerDoc["data"] = data;
            addTimestamp(largerDoc);
            if (!largerDoc.overflowed()) {
                doc = largerDoc;
            } else {
                char errorBuffer[192];
                if (context != nullptr) {
                    snprintf(errorBuffer, sizeof(errorBuffer), "[%s] 更大的文档仍然溢出 (大小: %d bytes)", context, largerSize);
                } else {
                    snprintf(errorBuffer, sizeof(errorBuffer), "[UnifiedAPI] 更大的文档仍然溢出 (大小: %d bytes)", largerSize);
                }
                if (logger != nullptr) {
                    logger->log("error", errorBuffer);
                }
            }
        } else {
            char errorBuffer[192];
            if (context != nullptr) {
                snprintf(errorBuffer, sizeof(errorBuffer), "[%s] JSON文档过大，无法重试 (需要: %d bytes)", context, largerSize);
            } else {
                snprintf(errorBuffer, sizeof(errorBuffer), "[UnifiedAPI] JSON文档过大，无法重试 (需要: %d bytes)", largerSize);
            }
            if (logger != nullptr) {
                logger->log("error", errorBuffer);
            }
        }
    }
    
    String output;
    output.reserve(estimatedSize);
    serializeJson(doc, output);
    
    // 如果仍然溢出，记录错误
    if (doc.overflowed()) {
        char buffer[192];
        if (context != nullptr) {
            snprintf(buffer, sizeof(buffer), "[%s] JSON文档溢出，无法序列化 (大小: %d bytes)", context, estimatedSize);
        } else {
            snprintf(buffer, sizeof(buffer), "[UnifiedAPI] JSON文档溢出，无法序列化 (大小: %d bytes)", estimatedSize);
        }
        if (logger != nullptr) {
            logger->log("error", buffer);
        }
    }
    
    return output;
}

String UnifiedAPI::createSuccessResponse(const String& data, const String& status, Logger* logger, const char* context) {
    size_t estimatedSize = 256 + data.length();
    DynamicJsonDocument doc(estimatedSize);
    doc["success"] = true;
    doc["status"] = status;
    if (data.length() > 0) {
        doc["data"] = data;
    }
    addTimestamp(doc);
    
    String output;
    output.reserve(estimatedSize);
    serializeJson(doc, output);
    return output;
}

String UnifiedAPI::createErrorResponse(const String& error, int code) {
    size_t estimatedSize = 256 + error.length();
    DynamicJsonDocument doc(estimatedSize);
    doc["success"] = false;
    doc["status"] = "error";
    doc["error"] = error;
    if (code > 0) {
        doc["code"] = code;
    }
    addTimestamp(doc);
    
    String output;
    output.reserve(estimatedSize);
    serializeJson(doc, output);
    return output;
}

String UnifiedAPI::createExecutingResponse(const String& message) {
    size_t estimatedSize = 256 + message.length();
    DynamicJsonDocument doc(estimatedSize);
    doc["success"] = true;
    doc["status"] = "executing";
    doc["data"] = message;
    addTimestamp(doc);
    
    String output;
    output.reserve(estimatedSize);
    serializeJson(doc, output);
    return output;
}

String UnifiedAPI::createStateResponse(const String& status, JsonObject& data) {
    size_t estimatedSize = 512;
    DynamicJsonDocument doc(estimatedSize);
    doc["success"] = true;
    doc["status"] = status;
    doc["data"] = data;
    addTimestamp(doc);
    
    String output;
    output.reserve(estimatedSize);
    serializeJson(doc, output);
    return output;
}

void UnifiedAPI::addTimestamp(DynamicJsonDocument& doc) {
    doc["timestamp"] = millis();
}
