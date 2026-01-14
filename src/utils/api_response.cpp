#include "api_response.h"
#include "logger.h"
#include "memory_monitor.h"

String APIResponse::success(const String& data, Logger* logger, const char* context) {
    return UnifiedAPI::createSuccessResponse(data, "success", logger, context);
}

String APIResponse::success(JsonObject& data, Logger* logger, const char* context) {
    return UnifiedAPI::createSuccessResponse(data, "success", logger, context);
}

String APIResponse::success(JsonArray& data, Logger* logger, const char* context) {
    return UnifiedAPI::createSuccessResponse(data, "success", logger, context);
}

String APIResponse::error(const String& message, int code) {
    return UnifiedAPI::createErrorResponse(message, code);
}

String APIResponse::executing(const String& message) {
    return UnifiedAPI::createExecutingResponse(message);
}

String APIResponse::fromJsonDocument(DynamicJsonDocument& doc, bool isSuccess) {
    // 如果文档已经有success字段，直接序列化
    if (doc.containsKey("success")) {
        String output;
        output.reserve(doc.memoryUsage());
        serializeJson(doc, output);
        return output;
    }
    
    // 否则添加success字段
    doc["success"] = isSuccess;
    String output;
    output.reserve(doc.memoryUsage());
    serializeJson(doc, output);
    return output;
}

bool APIResponse::parseResponse(const String& json, bool& success, String& data, String& error) {
    DynamicJsonDocument doc(512);
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        return false;
    }
    
    success = doc["success"] | false;
    if (doc.containsKey("data")) {
        if (doc["data"].is<const char*>()) {
            data = doc["data"].as<const char*>();
        } else {
            String temp;
            serializeJson(doc["data"], temp);
            data = temp;
        }
    } else {
        data = "";
    }
    
    if (doc.containsKey("error")) {
        error = doc["error"].as<const char*>();
    } else {
        error = "";
    }
    
    return true;
}

// APIResponseHandler 实现

bool APIResponseHandler::processAndSendResponse(const String& dataJson, const char* context, bool isArray, size_t maxDocSize) {
    if (dataJson.length() == 0) {
        return sendError("Insufficient memory", 503, context);
    }
    
    // 动态计算JSON大小：解析JSON需要的内存通常是字符串长度的1.5-2倍
    // 加上响应包装开销（success, status, timestamp, data字段）
    size_t docSize = (dataJson.length() * 2) + 1024;  // 2倍 + 1KB包装开销
    if (docSize < 2048) docSize = 2048;  // 最小2KB
    if (docSize > maxDocSize) docSize = maxDocSize;  // 限制最大大小
    
    DynamicJsonDocument dataDoc(docSize);
    DeserializationError err = deserializeJson(dataDoc, dataJson);
    if (err) {
        char errorMsg[128];
        const char* parseError = (err == DeserializationError::NoMemory) ? "No memory" : err.c_str();
        snprintf(errorMsg, sizeof(errorMsg), "Failed to parse response: %s", parseError);
        Logger* logger = getLogger();
        if (logger != nullptr) {
            logger->log("error", errorMsg);
        }
        return sendError("Failed to parse response", 500, context);
    }
    
    // 检查文档是否溢出
    if (dataDoc.overflowed()) {
        char buffer[192];
        snprintf(buffer, sizeof(buffer), "[%s] JSON文档溢出，尝试使用更大的文档 (当前大小: %d bytes, 数据长度: %d)", 
                 context ? context : "API", docSize, dataJson.length());
        Logger* logger = getLogger();
        if (logger != nullptr) {
            logger->log("warning", buffer);
        }
        
        // 尝试使用更大的文档重新解析
        size_t largerSize = dataJson.length() * 2 + 2048;
        if (largerSize <= maxDocSize) {
            DynamicJsonDocument largerDoc(largerSize);
            DeserializationError largerErr = deserializeJson(largerDoc, dataJson);
            if (!largerErr && !largerDoc.overflowed()) {
                dataDoc = largerDoc;
            } else {
                if (logger != nullptr) {
                    logger->log("error", "更大的文档仍然溢出或解析失败");
                }
                return sendError("JSON document overflowed", 500, context);
            }
        } else {
            if (logger != nullptr) {
                logger->log("error", "JSON文档过大，无法重试");
            }
            return sendError("JSON too large", 500, context);
        }
    }
    
    // 根据类型处理响应
    if (isArray) {
        JsonArray arr = dataDoc.as<JsonArray>();
        String output = APIResponse::success(arr, getLogger(), context);
        return sendSuccess(output, context);
    } else {
        JsonObject obj = dataDoc.as<JsonObject>();
        String output = APIResponse::success(obj, getLogger(), context);
        return sendSuccess(output, context);
    }
}

bool APIResponseHandler::processJsonArrayResponse(const String& dataJson, const char* context, size_t maxDocSize) {
    return processAndSendResponse(dataJson, context, true, maxDocSize);
}

bool APIResponseHandler::processJsonObjectResponse(const String& dataJson, const char* context, size_t maxDocSize) {
    return processAndSendResponse(dataJson, context, false, maxDocSize);
}
