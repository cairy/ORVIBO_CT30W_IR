#include "unified_api.h"

String UnifiedAPI::createSuccessResponse(JsonVariant data, const String& status) {
    size_t estimatedSize = 256;
    if (data.is<JsonObject>()) {
        estimatedSize += 512;
    } else if (data.is<JsonArray>()) {
        estimatedSize += 1024;
    } else if (data.is<const char*>()) {
        estimatedSize += strlen(data.as<const char*>());
    }
    
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

String UnifiedAPI::createSuccessResponse(const String& data, const String& status) {
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
