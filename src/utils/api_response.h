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

// 统一的响应处理器抽象类
// 用于统一 Web API 和 MQTT 的响应处理逻辑
class APIResponseHandler {
public:
    virtual ~APIResponseHandler() {}
    
    // 发送成功响应（纯虚函数，由子类实现）
    virtual bool sendSuccess(const String& json, const char* context = nullptr) = 0;
    
    // 发送错误响应（纯虚函数，由子类实现）
    virtual bool sendError(const String& error, int code = 0, const char* context = nullptr) = 0;
    
    // 获取日志记录器（可选）
    virtual Logger* getLogger() = 0;
    
    // 统一的 JSON 解析和响应发送流程
    // dataJson: APIHandlers 返回的 JSON 字符串（data 部分）
    // context: 上下文信息（如接口名称）
    // isArray: true 表示 JsonArray，false 表示 JsonObject
    // maxDocSize: 最大文档大小限制
    bool processAndSendResponse(const String& dataJson, const char* context, bool isArray, size_t maxDocSize = 16384);
    
    // 处理 JsonArray 响应
    bool processJsonArrayResponse(const String& dataJson, const char* context, size_t maxDocSize = 16384);
    
    // 处理 JsonObject 响应
    bool processJsonObjectResponse(const String& dataJson, const char* context, size_t maxDocSize = 16384);
};

#endif // API_RESPONSE_H
