#ifndef IR_MAPPER_H
#define IR_MAPPER_H

#include <Arduino.h>
#include "unified_signal.h"
#include "config/constants.h"
#include "utils/storage.h"

// 前向声明
#include <ArduinoJson.h>
class Logger;

// 映射规则
struct IR_MapRule {
    // A信号标识（使用统一信号结构）
    UnifiedSignal aSignal;
    
    // B信号序列（使用统一序列结构，包含信号数组、间隔数组和重复次数）
    SignalSequence bSequence;
    
    bool valid;
    uint8_t id;  // 规则ID
    char name[MAX_COMMAND_NAME_LEN];  // 可选名称（与按钮/指令对应，查询时便于识别）
};

// 映射表管理器
class IRMapper {
public:
    IRMapper();
    bool init();
    void setLogger(Logger* logger) { this->logger = logger; }  // 设置日志记录器（可选）
    bool initDefaultMappings();
    bool addRule(const IR_MapRule& rule, bool saveImmediately = true);
    bool updateRule(uint8_t id, const IR_MapRule& rule);
    bool deleteRule(uint8_t id);
    IR_MapRule* findRule(const UnifiedSignal& aSignal);
    bool isSignalInMappings(const UnifiedSignal& signal);  // 检查信号是否在映射表中（作为A信号或B信号）
    bool getRule(uint8_t id, IR_MapRule& rule);
    int getRuleCount() const;
    int getAllRules(IR_MapRule* rules, int maxCount);  // 返回实际填充的规则数量
    bool saveToFlash();
    bool loadFromFlash();
    bool exportToJSON(String& json);
    bool importFromJSON(const String& json);
    
    // 统一的JSON转换方法（供API使用）
    // 将规则转换为JSON对象（支持hex格式）
    static void ruleToJsonObject(const IR_MapRule& rule, JsonObject& obj, bool hexFormat = false);
    // 从JSON对象解析规则（支持hex格式）
    static bool jsonObjectToRule(JsonObject& obj, IR_MapRule& rule);

private:
    IR_MapRule rules[MAX_MAP_RULES];
    int ruleCount;  // 已废弃：仅保留用于向后兼容，不再用于遍历
    uint8_t nextId;
    Logger* logger;  // 日志记录器指针（可选）
    
    bool findRuleIndex(const UnifiedSignal& aSignal, int& index);
    bool findRuleIndexById(uint8_t id, int& index);
    
    // 内部辅助函数：找到第一个无效槽位
    int findEmptySlot() const;
    // 内部辅助函数：序列化规则到 JSON 数组
    int serializeRulesToJson(JsonArray& array);
    // 内部辅助函数：用更大的 JSON 文档重试序列化并保存
    bool saveToFlashWithLargerDoc(int validCount, size_t freeHeap);
};

#endif // IR_MAPPER_H

