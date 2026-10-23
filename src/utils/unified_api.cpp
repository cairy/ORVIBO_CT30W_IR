#include "unified_api.h"
#include "utils/logger.h"

String UnifiedAPI::createSuccessResponse(JsonVariant data, const String& status, Logger* logger, const char* context) {
    size_t estimatedSize = 256;  // 基础开销：success, status, timestamp等字段
    
    if (data.is<JsonObject>()) {
        // ⚠️ v1.3.52：原来写死 `+= 512` 是**错的**。对一个 30 字段的 status 对象，
        //   光键名 + 槽位就 >1.5 KB ⇒ **100% 溢出** ⇒ 走下面 largerDoc(1536) 把全部字段
        //   连字符串再深拷一遍，然后 `doc = largerDoc` 又是**拷贝赋值**
        //   （BasicJsonDocument::copyAssignFrom → reallocPool，**再申请一份等大池**）
        //   ⇒ 每个调用者都凭空多扛 ~3 KB 峰值，而设备空闲堆只有 ~5.6 KB。
        //   改成**按内容算**：序列化长度 + 键名/槽位开销。估小了也不会错 ——
        //   下面本来就有"溢出 ⇒ 开更大 doc 重试"的兜底。
        estimatedSize += measureJson(data) + 512;
    } else if (data.is<JsonArray>()) {
        // 按元素数量估算，避免完整序列化以获取长度（减少临时内存与碎片）
        JsonArray arr = data.as<JsonArray>();
        size_t n = arr.size();
        estimatedSize = 1024 + (n * 420);  // 包装开销 + 每条约 420 字节
        if (estimatedSize > 16384) estimatedSize = 16384;
        if (estimatedSize < 2048) estimatedSize = 2048;
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
    // 限制 reserve 上限以减少大块分配带来的内存碎片
    size_t reserveSize = (estimatedSize <= 8192) ? estimatedSize : 8192;
    output.reserve(reserveSize);
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
    output.reserve(estimatedSize <= 8192 ? estimatedSize : 8192);
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
    output.reserve(estimatedSize <= 8192 ? estimatedSize : 8192);
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
    output.reserve(estimatedSize <= 8192 ? estimatedSize : 8192);
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
