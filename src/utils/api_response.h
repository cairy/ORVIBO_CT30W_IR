#ifndef API_RESPONSE_H
#define API_RESPONSE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "unified_api.h"

// 前向声明
class Logger;

// 统一的API响应工具类（使用统一格式）
class APIResponse {
public:
    // 创建成功响应（统一格式）
    // logger: 可选的日志记录器，用于记录错误
    // context: 可选的上下文信息（如接口名称），用于溢出日志
    static String success(const String& data = "", Logger* logger = nullptr, const char* context = nullptr);
    static String success(JsonObject& data, Logger* logger = nullptr, const char* context = nullptr);
    static String success(JsonArray& data, Logger* logger = nullptr, const char* context = nullptr);
    
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
