#ifndef IR_MAPPER_H
#define IR_MAPPER_H

#include <Arduino.h>
#include "unified_signal.h"
#include "config/constants.h"
#include "utils/storage.h"

// 前向声明
#include <ArduinoJson.h>

// 映射规则
struct IR_MapRule {
    // A信号标识（使用统一信号结构）
    UnifiedSignal aSignal;
    
    // B信号序列（最多5个，使用统一信号结构）
    UnifiedSignal bSignals[MAX_B_SIGNALS_PER_RULE];
    uint8_t bSignalCount;
    
    // 配置参数
    uint16_t intervals[MAX_B_SIGNALS_PER_RULE - 1];  // 信号间隔（4个间隔对应5个信号）
    uint8_t repeatCount;  // 重复次数（1-10）
    
    bool valid;
    uint8_t id;  // 规则ID
};

// 映射表管理器
class IRMapper {
public:
    IRMapper();
    bool init();
    bool initDefaultMappings();
    bool addRule(const IR_MapRule& rule, bool saveImmediately = true);
    bool updateRule(uint8_t id, const IR_MapRule& rule);
    bool deleteRule(uint8_t id);
    IR_MapRule* findRule(const UnifiedSignal& aSignal);
    bool isSignalInMappings(const UnifiedSignal& signal);  // 检查信号是否在映射表中（作为A信号或B信号）
    bool getRule(uint8_t id, IR_MapRule& rule);
    int getRuleCount() const;
    bool getAllRules(IR_MapRule* rules, int maxCount);
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
    int ruleCount;
    uint8_t nextId;
    
    bool findRuleIndex(const UnifiedSignal& aSignal, int& index);
    bool findRuleIndexById(uint8_t id, int& index);
};

#endif // IR_MAPPER_H

