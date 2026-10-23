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
    // ⚠️ 三个增删接口的 `saveImmediately` 是**批量写入的开关**（v1.3.18）：
    // 传 false 时只改内存，由调用方在循环**结束后统一调一次 saveToFlash()**。
    // 原因是每条各自落盘 = 一次全量文件重写，而 Web 面板"保存"走的是全量替换
    // （先逐条 delete 再逐条 add），5 条规则实测要 1.937s —— 因为写了 10 次 flash。
    bool addRule(const IR_MapRule& rule, bool saveImmediately = true);
    bool updateRule(uint8_t id, const IR_MapRule& rule, bool saveImmediately = true);
    bool deleteRule(uint8_t id, bool saveImmediately = true);
    IR_MapRule* findRule(const UnifiedSignal& aSignal);
    bool isSignalInMappings(const UnifiedSignal& signal);  // 检查信号是否在映射表中（作为A信号或B信号）
    bool getRule(uint8_t id, IR_MapRule& rule);
    // 按 ID 取规则指针（不复制）。给"先把规则记下来、过一会儿再发"的延迟路径用：
    // 记裸指针跨 20ms 有风险（这期间 Web 面板可能把规则删了/改了，指针就悬空了），
    // 记 ID 在这个函数里回查则天然安全 —— 规则没了就返回 nullptr。
    // 不要用 getRule()：那是整条规则的按值拷贝（含 Base64 String 与间隔数组，KB 级）。
    IR_MapRule* findRuleById(uint8_t id);
    int getRuleCount() const;
    int getAllRules(IR_MapRule* rules, int maxCount);  // 返回实际填充的规则数量
    // 只把现有规则的 id 抄出来（返回个数）。全量替换时的"先全删"只需要 id，
    // 用 getAllRules 会把整条规则都复制一份（20 条 ≈ 几 KB），在配了 MQTT
    // （空闲 3~5 KB）时几乎必然分配失败 —— 见 api_handlers.cpp 里那段注释。
    // 这里用调用方的栈数组（MAX_MAP_RULES 字节），不可能失败。
    int collectIds(uint8_t* out, int maxCount) const;
    bool saveToFlash();
    bool loadFromFlash();
    // 上一次 loadFromFlash() 是不是"文件在、内容可能是好的、但我们读不出来"
    // （内存不够 / JSON 文档溢出 / 打不开文件）。这种失败**绝不能**被
    // initDefaultMappings() 用 5 条默认规则覆盖掉 —— 那等于一次开机就把用户
    // 的 20 条规则不可逆地清成默认值。见 main.cpp 里对它的判断。
    bool hadLoadFailure() const { return loadFailed; }
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
    bool loadFailed;  // 见 hadLoadFailure()
    
    bool findRuleIndex(const UnifiedSignal& aSignal, int& index);
    bool findRuleIndexById(uint8_t id, int& index);
    
    // 内部辅助函数：找到第一个无效槽位
    int findEmptySlot() const;
    // 取一个未被占用、且**永不为 0** 的规则 ID，并推进 nextId。
    // 原实现是裸的 `nextId++`（uint8_t）：加到 255 会回绕成 0，而 0 在本模块各处
    // 都被当成"非法/无效"（如 deleteRule 里的 `if (id != 0)`）⇒ 那条规则**永远删不掉**。
    uint8_t allocateId();
};

#endif // IR_MAPPER_H

