#ifndef API_RESPONSE_H
#define API_RESPONSE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "unified_api.h"

// 统一的API响应工具类（使用统一格式）
class APIResponse {
public:
    // 创建成功响应（统一格式）
    static String success(const String& data = "");
    static String success(JsonObject& data);
    static String success(JsonArray& data);
    
    // 创建错误响应（统一格式）
    static String error(const String& message, int code = 0);
    
    // 创建执行中响应（统一格式）
    static String executing(const String& message);
    
    // 从JSON文档创建响应（用于复杂数据结构）
    static String fromJsonDocument(DynamicJsonDocument& doc, bool isSuccess = true);
    
    // 解析响应（用于测试）
    static bool parseResponse(const String& json, bool& success, String& data, String& error);
};

#endif // API_RESPONSE_H
