#ifndef UNIFIED_API_H
#define UNIFIED_API_H

#include <Arduino.h>
#include <ArduinoJson.h>

// 前向声明
class Logger;

// 统一API响应工具类
class UnifiedAPI {
public:
    // 创建成功响应
    // logger: 可选的日志记录器，用于记录错误
    // context: 可选的上下文信息（如接口名称），用于溢出日志
    static String createSuccessResponse(JsonVariant data, const String& status = "success", Logger* logger = nullptr, const char* context = nullptr);
    static String createSuccessResponse(const String& data, const String& status = "success", Logger* logger = nullptr, const char* context = nullptr);
    
    // 创建错误响应
    static String createErrorResponse(const String& error, int code = 0);
    
    // 创建执行中响应
    static String createExecutingResponse(const String& message);
    
    // 创建状态响应（用于publishEnhancedState）
    static String createStateResponse(const String& status, JsonObject& data);
    
    // 辅助方法：创建带时间戳的响应
    static void addTimestamp(DynamicJsonDocument& doc);
};

#endif // UNIFIED_API_H
