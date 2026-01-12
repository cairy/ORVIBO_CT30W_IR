#ifndef UNIFIED_API_H
#define UNIFIED_API_H

#include <Arduino.h>
#include <ArduinoJson.h>

// 统一API响应工具类
class UnifiedAPI {
public:
    // 创建成功响应
    static String createSuccessResponse(JsonVariant data, const String& status = "success");
    static String createSuccessResponse(const String& data, const String& status = "success");
    
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
