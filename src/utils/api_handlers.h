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
    // 默认：带 rules ⇒ 批量替换（先删光再写）；带 id+rule ⇒ 单条更新。
    // 可选 mode："append" = 只追加（带 rules，重复 A 信号/满/非法计入 skipped）；
    //           "delete" = 按 id 或 ids[] 删除。HTTP 与 MQTT 共用本函数。
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
    // 查询学习结果。成功返回 data（非空）；失败返回空串，并把**具体原因**写进
    // outcome —— 以前三种失败（没学过 / 内存不足 / 转换失败）全折叠成空串，
    // 两个调用方（HTTP、MQTT）一律映射成 404 "没有学习结果"。于是设备只是当时
    // 内存不够时，用户看到的是"没有学习结果"，照着重学一遍还是同样的结果，
    // 是一条**不可自愈的假诊断**。
    enum class LearnQueryStatus {
        OK,
        NO_DATA,          // 确实没有学习结果 → 404
        LOW_MEMORY,       // 当时内存不够 → 503，等一会儿重试即可
        CONVERT_FAILED    // 有结果但转换/序列化失败 → 500，是缺陷可上报
    };
    static String handleQueryLearnResult(IRLearner& learner, const String& format = "string",
                                         LearnQueryStatus* outcome = nullptr);
    
    // 获取最近信号（返回JSON字符串，data部分）
    // 成功返回JSON字符串，失败返回空字符串（需要检查内存）
    // since > 0 时只返回 timestamp 严格大于 since 的新条目（供前端增量轮询用）
    static String handleGetRecentSignals(Logger& logger, unsigned long since = 0);
    
    // 发送信号
    // 返回: 成功返回消息字符串，失败返回错误消息
    // success: 输出参数，表示是否成功
    // 把各入口的信号 payload 归一成 forwarder 认的形式（字符串原样；JSON 里的
    // signal 单条包装成 signals 序列）。HTTP / MQTT / SmartIR 三条入口共用这一份，
    // 避免"网页上能发、MQTT 上发不出去"这类只跟入口有关的差异。
    static String normalizeSignalPayload(const String& payload);
    static String handleSendSignal(IRForwarder& forwarder, const String& payload, bool& success);
    
    // 获取转发记录（返回JSON数组字符串；内存不足返回空字符串）
    static String handleGetForwardRecords(IRForwarder& forwarder);
    
    // 试跑一条 A 信号：立刻匹配规则并发射，返回人可读的结论
    // payload 可以是 {"a":"1,0x00003039"}，也可以是纯字符串 "1,0x00003039"
    static String handleForwardTest(IRForwarder& forwarder, const String& payload, bool& success);
};

#endif // API_HANDLERS_H
