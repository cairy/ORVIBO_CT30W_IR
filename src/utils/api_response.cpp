#include "api_response.h"

String APIResponse::success(const String& data) {
    DynamicJsonDocument doc(256 + data.length());
    doc["success"] = true;
    if (data.length() > 0) {
        doc["data"] = data;
    }
    String output;
    output.reserve(256 + data.length());
    serializeJson(doc, output);
    return output;
}

String APIResponse::success(JsonObject& data) {
    DynamicJsonDocument doc(512);
    doc["success"] = true;
    doc["data"] = data;
    String output;
    output.reserve(512);
    serializeJson(doc, output);
    return output;
}

String APIResponse::success(JsonArray& data) {
    DynamicJsonDocument doc(512);
    doc["success"] = true;
    doc["data"] = data;
    String output;
    output.reserve(512);
    serializeJson(doc, output);
    return output;
}

String APIResponse::error(const String& message, int code) {
    DynamicJsonDocument doc(256 + message.length());
    doc["success"] = false;
    doc["error"] = message;
    if (code > 0) {
        doc["code"] = code;
    }
    String output;
    output.reserve(256 + message.length());
    serializeJson(doc, output);
    return output;
}

String APIResponse::executing(const String& message) {
    DynamicJsonDocument doc(256 + message.length());
    doc["success"] = true;
    doc["status"] = "executing";
    doc["message"] = message;
    String output;
    output.reserve(256 + message.length());
    serializeJson(doc, output);
    return output;
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
