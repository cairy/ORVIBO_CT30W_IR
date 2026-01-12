#include "api_response.h"

String APIResponse::success(const String& data) {
    return UnifiedAPI::createSuccessResponse(data, "success");
}

String APIResponse::success(JsonObject& data) {
    return UnifiedAPI::createSuccessResponse(data, "success");
}

String APIResponse::success(JsonArray& data) {
    return UnifiedAPI::createSuccessResponse(data, "success");
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
