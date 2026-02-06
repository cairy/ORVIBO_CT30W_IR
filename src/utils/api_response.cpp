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
    
    // 检查最大连续块大小，避免内存碎片化导致分配失败
    size_t maxBlock = MemoryMonitor::getLargestFreeBlock();
    size_t freeHeap = MemoryMonitor::getFreeHeap();
    
    // 动态计算JSON大小：解析JSON需要的内存通常是字符串长度的2-3倍
    // 对于嵌套对象和数组，ArduinoJson需要更多内存（每个对象、数组、字符串都需要额外内存）
    // 加上响应包装开销（success, status, timestamp, data字段）
    size_t docSize = (dataJson.length() * 3) + 2048;  // 3倍 + 2KB包装开销（增加缓冲）
    if (docSize < 4096) docSize = 4096;  // 最小4KB
    if (docSize > maxDocSize) docSize = maxDocSize;  // 限制最大大小
    
    // 如果最大连续块不够，使用最大连续块大小（减去安全余量）
    if (maxBlock < docSize + 512) {
        if (maxBlock > 2048) {
            docSize = maxBlock - 512;  // 使用最大连续块，减去安全余量
            if (docSize > maxDocSize) docSize = maxDocSize;
        } else {
            // 内存严重碎片化，无法分配足够大的文档
            Logger* logger = getLogger();
            if (logger != nullptr) {
                char errorMsg[192];
                snprintf(errorMsg, sizeof(errorMsg), "[%s] 内存碎片化严重，无法分配JSON文档 (需要: %d bytes, 最大连续块: %d bytes, 空闲堆: %d bytes)", 
                         context ? context : "API", docSize, maxBlock, freeHeap);
                logger->log("error", errorMsg);
            }
            return sendError("Insufficient memory (fragmented)", 503, context);
        }
    }
    
    DynamicJsonDocument dataDoc(docSize);
    DeserializationError err = deserializeJson(dataDoc, dataJson);
    if (err) {
        char errorMsg[192];
        const char* parseError = (err == DeserializationError::NoMemory) ? "No memory" : err.c_str();
        snprintf(errorMsg, sizeof(errorMsg), "Failed to parse response: %s (docSize: %d, dataLength: %d, maxBlock: %d)", 
                 parseError, docSize, dataJson.length(), maxBlock);
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
        
        // 尝试使用更大的文档重新解析：使用最大允许大小
        if (maxDocSize > docSize) {
            DynamicJsonDocument largerDoc(maxDocSize);
            DeserializationError largerErr = deserializeJson(largerDoc, dataJson);
            if (!largerErr && !largerDoc.overflowed()) {
                dataDoc = largerDoc;
                if (logger != nullptr) {
                    char successMsg[192];
                    snprintf(successMsg, sizeof(successMsg), "[%s] 使用最大文档成功解析 (大小: %d bytes)", 
                             context ? context : "API", maxDocSize);
                    logger->log("system", successMsg);
                }
            } else {
                if (logger != nullptr) {
                    char errorMsg[192];
                    snprintf(errorMsg, sizeof(errorMsg), "[%s] 最大文档仍然溢出或解析失败 (大小: %d bytes, 数据长度: %d)", 
                             context ? context : "API", maxDocSize, dataJson.length());
                    logger->log("error", errorMsg);
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
    if (dataJson.length() == 0) {
        return sendError("Insufficient memory", 503, context);
    }
    
    // 优化：如果 dataJson 已经是 JSON 数组字符串，直接字符串拼接包装，避免解析
    // 这样可以避免 ArduinoJson 解析时的大量内存需求
    String trimmed = dataJson;
    trimmed.trim();
    if (trimmed.length() > 0 && trimmed[0] == '[') {
        // 直接字符串拼接构建完整响应，避免解析 JSON
        String output;
        output.reserve(trimmed.length() + 256);  // 预分配内存
        output = "{\"success\":true,\"status\":\"success\",\"data\":";
        output += trimmed;
        output += ",\"timestamp\":";
        output += String(millis());
        output += "}";
        
        return sendSuccess(output, context);
    }
    
    // 如果不是 JSON 数组格式，使用传统方法解析
    return processAndSendResponse(dataJson, context, true, maxDocSize);
}

bool APIResponseHandler::processJsonObjectResponse(const String& dataJson, const char* context, size_t maxDocSize) {
    return processAndSendResponse(dataJson, context, false, maxDocSize);
}
