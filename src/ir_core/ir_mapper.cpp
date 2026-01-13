#include "ir_mapper.h"
#include <ArduinoJson.h>
#include "utils/hex_utils.h"
#include "unified_signal.h"

IRMapper::IRMapper() : ruleCount(0), nextId(1) {
    for (int i = 0; i < MAX_MAP_RULES; i++) {
        rules[i].valid = false;
    }
}

bool IRMapper::init() {
    return loadFromFlash();
}

bool IRMapper::initDefaultMappings() {
    // 如果映射表不为空，直接返回
    if (ruleCount > 0) {
        return true;
    }
    
    // 检查可用内存，如果内存不足则跳过初始化
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 4096) {  // 至少需要4KB空闲内存来保存
        Serial.printf("[IRMapper] 内存不足，跳过默认映射初始化 (空闲: %d bytes)\n", freeHeap);
        return false;
    }
    
    // 从旧程序提取的映射关系常量
    const uint32_t NEC_PROTOCOL = 1;  // NEC协议
    
    // A信号（接收端）
    const uint32_t SOURCE_VOLUP = 0x11B08F7;
    const uint32_t SOURCE_VOLDOWN = 0x11B8877;
    const uint32_t SOURCE_MUTE = 0x11B708F;
    const uint32_t SOURCE_POWER = 0x11B26D9;
    const uint32_t SOURCE_AVTV = 0x11B7689;
    
    // B信号（发送端）
    const uint32_t TARGET_VOLUP = 0xFD22DD;
    const uint32_t TARGET_VOLDOWN = 0xFDC23D;
    const uint32_t TARGET_MUTE = 0xFD708F;
    const uint32_t TARGET_POWER = 0xFDB04F;
    const uint32_t TARGET_AVTV = 0xFD48B7;
    
    // 创建映射规则
    IR_MapRule rule;
    
    // 初始化间隔数组
    for (int i = 0; i < MAX_B_SIGNALS_PER_RULE - 1; i++) {
        rule.intervals[i] = 0;
    }
    
    // 1. 音量+
    rule.aSignal.protocol = NEC_PROTOCOL;
    rule.aSignal.data = SOURCE_VOLUP;
    rule.aSignal.isProtocol = true;
    rule.aSignal.useTiming = false;
    rule.aSignal.repeatCount = 1;
    
    rule.bSignalCount = 1;
    rule.bSignals[0].protocol = NEC_PROTOCOL;
    rule.bSignals[0].data = TARGET_VOLUP;
    rule.bSignals[0].isProtocol = true;
    rule.bSignals[0].useTiming = false;
    rule.bSignals[0].repeatCount = 1;
    
    rule.repeatCount = 1;
    rule.valid = true;
    
    // 批量添加规则，不立即保存（节省内存）
    if (!addRule(rule, false)) {
        return false;
    }
    
    // 2. 音量-
    rule.aSignal.data = SOURCE_VOLDOWN;
    rule.bSignals[0].data = TARGET_VOLDOWN;
    if (!addRule(rule, false)) {
        return false;
    }
    
    // 3. 静音
    rule.aSignal.data = SOURCE_MUTE;
    rule.bSignals[0].data = TARGET_MUTE;
    if (!addRule(rule, false)) {
        return false;
    }
    
    // 4. 电源（需要重复3次）
    rule.aSignal.data = SOURCE_POWER;
    rule.bSignals[0].data = TARGET_POWER;
    rule.repeatCount = 3;
    if (!addRule(rule, false)) {
        return false;
    }
    
    // 5. AV/TV
    rule.aSignal.data = SOURCE_AVTV;
    rule.bSignals[0].data = TARGET_AVTV;
    rule.repeatCount = 1;
    if (!addRule(rule, false)) {
        return false;
    }
    
    // 所有规则添加完成后，一次性保存
    return saveToFlash();
}

bool IRMapper::addRule(const IR_MapRule& rule, bool saveImmediately) {
    if (ruleCount >= MAX_MAP_RULES) {
        return false;
    }
    
    // 检查是否已存在相同的A信号
    int existingIndex;
    if (findRuleIndex(rule.aSignal, existingIndex)) {
        return false;  // 已存在
    }
    
    rules[ruleCount] = rule;
    rules[ruleCount].id = nextId++;
    rules[ruleCount].valid = true;
    ruleCount++;
    
    if (saveImmediately) {
        saveToFlash();
    }
    return true;
}

bool IRMapper::updateRule(uint8_t id, const IR_MapRule& rule) {
    int index;
    if (!findRuleIndexById(id, index)) {
        return false;
    }
    
    rules[index] = rule;
    rules[index].id = id;  // 保持原有ID
    rules[index].valid = true;
    
    saveToFlash();
    return true;
}

bool IRMapper::deleteRule(uint8_t id) {
    int index;
    if (!findRuleIndexById(id, index)) {
        return false;
    }
    
    // 移动后续规则
    for (int i = index; i < ruleCount - 1; i++) {
        rules[i] = rules[i + 1];
    }
    
    ruleCount--;
    rules[ruleCount].valid = false;
    
    saveToFlash();
    return true;
}

IR_MapRule* IRMapper::findRule(const UnifiedSignal& aSignal) {
    int index;
    if (findRuleIndex(aSignal, index)) {
        return &rules[index];
    }
    return nullptr;
}

bool IRMapper::isSignalInMappings(const UnifiedSignal& signal) {
    // 只检查B信号（A信号应该已经通过findRule检查过了）
    for (int i = 0; i < ruleCount; i++) {
        if (!rules[i].valid) continue;
        
        // 检查B信号
        for (int j = 0; j < rules[i].bSignalCount; j++) {
            if (SignalUtils::signalsEqual(signal, rules[i].bSignals[j])) {
                return true;
            }
        }
    }
    
    return false;
}

bool IRMapper::getRule(uint8_t id, IR_MapRule& rule) {
    int index;
    if (!findRuleIndexById(id, index)) {
        return false;
    }
    
    rule = rules[index];
    return true;
}

int IRMapper::getRuleCount() const {
    return ruleCount;
}

bool IRMapper::getAllRules(IR_MapRule* outputRules, int maxCount) {
    if (outputRules == nullptr || maxCount <= 0) {
        return false;
    }
    
    int count = (maxCount < ruleCount) ? maxCount : ruleCount;
    for (int i = 0; i < count; i++) {
        outputRules[i] = rules[i];
    }
    
    return true;
}

bool IRMapper::saveToFlash() {
    // 检查可用内存
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 4096) {  // 至少需要4KB空闲内存
        Serial.printf("[IRMapper] 内存不足，跳过保存 (空闲: %d bytes)\n", freeHeap);
        return false;
    }
    
    // 动态计算JSON大小：每条规则约200字节，加上JSON开销
    size_t jsonSize = (ruleCount * 200) + 2048;
    if (jsonSize < 4096) jsonSize = 4096;  // 最小4KB
    
    // 确保不超过可用内存
    if (jsonSize > freeHeap - 1024) {  // 保留1KB缓冲
        jsonSize = freeHeap - 1024;
        if (jsonSize < 1024) {
            Serial.printf("[IRMapper] 内存不足，无法保存 (需要至少1KB)\n");
            return false;
        }
    }
    
    DynamicJsonDocument doc(jsonSize);
    JsonArray rulesArray = doc.to<JsonArray>();
    
    for (int i = 0; i < ruleCount; i++) {
        if (!rules[i].valid) continue;
        
        JsonObject ruleObj = rulesArray.createNestedObject();
        // 使用统一方法，设置hexFormat=true，统一使用十六进制格式
        ruleToJsonObject(rules[i], ruleObj, true);
    }
    
    String output;
    serializeJson(doc, output);
    return Storage::saveFile(CONFIG_FILE_MAPS, output);
}

bool IRMapper::loadFromFlash() {
    String content = Storage::readFile(CONFIG_FILE_MAPS);
    
    // 情况1：文件不存在，创建空数组文件
    if (content.length() == 0) {
        Serial.println("[IRMapper] Maps文件不存在，创建空文件");
        if (Storage::saveFile(CONFIG_FILE_MAPS, "[]")) {
            ruleCount = 0;
            nextId = 1;
            return true;
        }
        return false;
    }
    
    // 动态计算JSON大小：文件大小加上解析开销
    size_t jsonSize = content.length() + 1024;
    if (jsonSize < 4096) jsonSize = 4096;
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, content);
    
    // 情况2：JSON解析失败（可能是旧格式或损坏）
    if (error) {
        Serial.println("[IRMapper] Maps文件格式错误，重置为空文件");
        if (Storage::saveFile(CONFIG_FILE_MAPS, "[]")) {
            ruleCount = 0;
            nextId = 1;
            return true;
        }
        return false;
    }
    
    JsonArray rulesArray = doc.as<JsonArray>();
    ruleCount = 0;
    nextId = 1;
    bool hasValidRules = false;
    int originalRuleCount = rulesArray.size();
    
    // 尝试加载规则
    for (JsonObject ruleObj : rulesArray) {
        if (ruleCount >= MAX_MAP_RULES) break;
        
        IR_MapRule& rule = rules[ruleCount];
        // 使用统一方法解析（仅支持十六进制字符串格式）
        if (!jsonObjectToRule(ruleObj, rule)) {
            // 解析失败，可能是旧格式
            continue;
        }
        
        rule.id = ruleObj["id"] | 0;
        if (rule.id >= nextId) {
            nextId = rule.id + 1;
        }
        
        ruleCount++;
        hasValidRules = true;
    }
    
    // 情况3：文件存在但所有规则都是旧格式（解析失败）
    if (!hasValidRules && originalRuleCount > 0) {
        Serial.println("[IRMapper] 检测到旧格式数据，重置为空文件");
        if (Storage::saveFile(CONFIG_FILE_MAPS, "[]")) {
            ruleCount = 0;
            nextId = 1;
            return true;
        }
        return false;
    }
    
    return true;
}

bool IRMapper::exportToJSON(String& json) {
    // 动态计算JSON大小：每条规则约200字节
    size_t jsonSize = (ruleCount * 200) + 2048;
    if (jsonSize < 4096) jsonSize = 4096;
    DynamicJsonDocument doc(jsonSize);
    JsonArray rulesArray = doc.to<JsonArray>();
    
    // 使用统一方法，设置hexFormat=true，统一使用十六进制格式（与saveToFlash一致）
    for (int i = 0; i < ruleCount; i++) {
        if (!rules[i].valid) continue;
        
        JsonObject ruleObj = rulesArray.createNestedObject();
        ruleToJsonObject(rules[i], ruleObj, true);  // true表示使用hex格式
    }
    
    serializeJson(doc, json);
    return true;
}

bool IRMapper::importFromJSON(const String& json) {
    // 动态计算JSON大小：JSON字符串长度加上解析开销
    size_t jsonSize = json.length() + 1024;
    if (jsonSize < 4096) jsonSize = 4096;
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, json);
    if (error) {
        return false;
    }
    
    // 清空现有规则
    ruleCount = 0;
    nextId = 1;
    
    JsonArray rulesArray = doc.as<JsonArray>();
    for (JsonObject ruleObj : rulesArray) {
        if (ruleCount >= MAX_MAP_RULES) break;
        
        IR_MapRule rule;
        // 使用统一方法解析（仅支持十六进制字符串格式）
        if (!jsonObjectToRule(ruleObj, rule)) {
            continue;  // 解析失败，跳过这条规则
        }
        
        rule.id = ruleObj["id"] | 0;
        rules[ruleCount++] = rule;
        if (rule.id >= nextId) {
            nextId = rule.id + 1;
        }
    }
    
    return saveToFlash();
}

bool IRMapper::findRuleIndex(const UnifiedSignal& aSignal, int& index) {
    for (int i = 0; i < ruleCount; i++) {
        if (!rules[i].valid) continue;
        
        if (SignalUtils::signalsEqual(aSignal, rules[i].aSignal)) {
            index = i;
            return true;
        }
    }
    
    return false;
}

bool IRMapper::findRuleIndexById(uint8_t id, int& index) {
    for (int i = 0; i < ruleCount; i++) {
        if (rules[i].valid && rules[i].id == id) {
            index = i;
            return true;
        }
    }
    return false;
}

// 统一的规则转JSON方法
void IRMapper::ruleToJsonObject(const IR_MapRule& rule, JsonObject& obj, bool hexFormat) {
    obj["id"] = rule.id;
    
    // A信号（使用统一序列化方法）
    JsonObject aSignal = obj.createNestedObject("aSignal");
    SignalUtils::signalToJSON(rule.aSignal, aSignal, hexFormat);
    
    // B信号序列（使用统一序列化方法）
    JsonArray bSignals = obj.createNestedArray("bSignals");
    for (int j = 0; j < rule.bSignalCount; j++) {
        JsonObject bSignal = bSignals.createNestedObject();
        SignalUtils::signalToJSON(rule.bSignals[j], bSignal, hexFormat);
    }
    
    obj["bSignalCount"] = rule.bSignalCount;
    
    // 间隔
    JsonArray intervals = obj.createNestedArray("intervals");
    for (int j = 0; j < rule.bSignalCount - 1; j++) {
        intervals.add(rule.intervals[j]);
    }
    
    obj["repeatCount"] = rule.repeatCount;
}

// 统一的JSON转规则方法
bool IRMapper::jsonObjectToRule(JsonObject& obj, IR_MapRule& rule) {
    rule.id = obj["id"] | 0;
    
    // A信号（使用统一反序列化方法）
    JsonObject aSignal = obj["aSignal"];
    if (aSignal.isNull()) {
        return false;
    }
    if (!SignalUtils::signalFromJSON(aSignal, rule.aSignal)) {
        return false;
    }
    
    // B信号序列（使用统一反序列化方法）
    JsonArray bSignals = obj["bSignals"];
    rule.bSignalCount = 0;
    for (JsonObject bSignal : bSignals) {
        if (rule.bSignalCount >= MAX_B_SIGNALS_PER_RULE) break;
        
        if (!SignalUtils::signalFromJSON(bSignal, rule.bSignals[rule.bSignalCount])) {
            return false;
        }
        rule.bSignalCount++;
    }
    
    // 间隔
    JsonArray intervals = obj["intervals"];
    int intervalCount = 0;
    for (uint16_t interval : intervals) {
        if (intervalCount >= MAX_B_SIGNALS_PER_RULE - 1) break;
        rule.intervals[intervalCount++] = interval;
    }
    
    rule.repeatCount = obj["repeatCount"] | 1;
    rule.valid = true;
    
    return true;
}
