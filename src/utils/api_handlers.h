#ifndef API_HANDLERS_H
#define API_HANDLERS_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "ir_core/ir_mapper.h"
#include "ir_core/ir_learner.h"
#include "ir_core/ir_forwarder.h"
#include "utils/logger.h"
#include "utils/memory_monitor.h"
#include "utils/hex_utils.h"
#include "config/constants.h"

// 公共API业务逻辑处理类
// 提取HTTP和MQTT共用的业务逻辑，提升代码可维护性
class APIHandlers {
public:
    // 查询映射规则（返回JSON字符串，data部分）
    // 成功返回JSON字符串，失败返回空字符串（需要检查内存）
    // logger: 可选的日志记录器，用于记录错误
    static String handleQueryMaps(IRMapper& mapper, Logger* logger = nullptr);
    
    // 更新映射规则
    // 返回: 成功返回消息字符串，失败返回错误消息
    // success: 输出参数，表示是否成功
    static String handleUpdateMap(IRMapper& mapper, const String& json, bool& success);
    
    // 获取日志（返回JSON字符串，data部分）
    // 成功返回JSON字符串，失败返回空字符串（需要检查内存）
    static String handleGetLogs(Logger& logger);
    
    // 启动学习
    // 返回: 成功返回消息字符串，失败返回错误消息
    // success: 输出参数，表示是否成功
    static String handleStartLearn(IRLearner& learner, const String& json, bool& success);
    
    // 查询学习状态（返回JSON字符串，data部分，简化版）
    // 只返回: isLearning, progress, currentCommand
    // 成功返回JSON字符串，失败返回空字符串（需要检查内存）
    static String handleQueryLearnStatus(IRLearner& learner);
    
    // 停止学习
    // 返回: 成功返回消息字符串，失败返回错误消息
    // success: 输出参数，表示是否成功
    static String handleStopLearn(IRLearner& learner, bool& success);
    
    // 查询学习结果（返回JSON字符串，data部分）
    // format: "base64"（默认）, "signalid", "array"
    // 成功返回JSON字符串，失败返回空字符串（需要检查内存）
    static String handleQueryLearnResult(IRLearner& learner, const String& format = "base64");
    
    // 获取最近信号（返回JSON字符串，data部分）
    // 成功返回JSON字符串，失败返回空字符串（需要检查内存）
    static String handleGetRecentSignals(Logger& logger);
    
    // 发送信号
    // 返回: 成功返回消息字符串，失败返回错误消息
    // success: 输出参数，表示是否成功
    static String handleSendSignal(IRForwarder& forwarder, const String& payload, bool& success);
};

#endif // API_HANDLERS_H
