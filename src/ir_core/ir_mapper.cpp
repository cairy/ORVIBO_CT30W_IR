#include "ir_mapper.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "utils/hex_utils.h"
#include "utils/logger.h"
#include "unified_signal.h"

IRMapper::IRMapper() : ruleCount(0), nextId(1), logger(nullptr) {
    for (int i = 0; i < MAX_MAP_RULES; i++) {
        rules[i].valid = false;
    }
}

bool IRMapper::init() {
    return loadFromFlash();
}

bool IRMapper::initDefaultMappings() {
    // 如果映射表不为空，直接返回（使用getRuleCount确保准确性）
    if (getRuleCount() > 0) {
        return true;
    }
    
    // 从旧程序提取的映射关系常量
    const uint32_t NEC_PROTOCOL = 1;  // NEC协议
    
    // A信号（接收端）
    const uint32_t SOURCE_VOLUP = 0x11B08F7;
    const uint32_t SOURCE_VOLDOWN = 0x11B8877;
    const uint32_t SOURCE_MUTE = 0x11B708F;
    const uint32_t SOURCE_POWER = 0x11B26D9;
    const uint32_t SOURCE_AVTV = 0x11B7689;
    
    // 检查可用内存，如果内存不足则跳过初始化
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 4096) {  // 至少需要4KB空闲内存来保存
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "内存不足，跳过默认映射初始化 (空闲: %d bytes)", freeHeap);
        if (logger) {
            logger->log("error", buffer);
        } else {
            Serial.printf("[IRMapper] %s\n", buffer);
        }
        return false;
    }
    
    // B信号（发送端）
    const uint32_t TARGET_VOLUP = 0xFD22DD;
    const uint32_t TARGET_VOLDOWN = 0xFDC23D;
    const uint32_t TARGET_MUTE = 0xFD708F;
    const uint32_t TARGET_POWER = 0xFDB04F;
    const uint32_t TARGET_AVTV = 0xFD48B7;
    
    // 创建映射规则
    IR_MapRule rule;
    
    // 定义所有默认规则
    struct DefaultRule {
        uint32_t sourceData;
        uint32_t targetData;
        uint8_t repeatCount;
        const char* name;
    };
    
    DefaultRule defaultRules[] = {
        {SOURCE_VOLUP, TARGET_VOLUP, 1, "音量+"},
        {SOURCE_VOLDOWN, TARGET_VOLDOWN, 1, "音量-"},
        {SOURCE_MUTE, TARGET_MUTE, 1, "静音"},
        {SOURCE_POWER, TARGET_POWER, 1, "电源"},  // repeatCount=1，因为序列中已经包含了3个信号
        {SOURCE_AVTV, TARGET_AVTV, 1, "AV/TV"}
    };
    
    // 初始化基础规则结构
    rule.aSignal.protocol = NEC_PROTOCOL;
    rule.aSignal.useTiming = false;  // 协议信号
    rule.aSignal.repeatCount = 1;
    
    rule.bSequence.clear();
    rule.bSequence.repeatCount = 1;
    rule.valid = true;
    
    // 批量添加规则，不立即保存（节省内存）
    int successCount = 0;
    int totalRules = sizeof(defaultRules) / sizeof(defaultRules[0]);
    
    for (int i = 0; i < totalRules; i++) {
        rule.aSignal.data = defaultRules[i].sourceData;
        
        // 构建B信号序列
        rule.bSequence.clear();
        
        // 特殊处理电源规则：包含三个B信号（完整信号 + 2次重复码）
        // 参考 ir-converter-finally.ino：完整信号 + 延时30ms + 重复码 + 延时70ms + 重复码
        if (defaultRules[i].sourceData == SOURCE_POWER) {
            // 第一个B信号：完整信号 0xFDB04F
            UnifiedSignal bSignal1;
            bSignal1.protocol = NEC_PROTOCOL;
            bSignal1.useTiming = false;
            bSignal1.data = TARGET_POWER;  // 0xFDB04F
            bSignal1.repeatCount = 1;
            rule.bSequence.addSignal(bSignal1, 0);
            
            // 第二个B信号：第一次重复码 0xFFFFFFFF，间隔30ms
            UnifiedSignal bSignal2;
            bSignal2.protocol = NEC_PROTOCOL;
            bSignal2.useTiming = false;
            bSignal2.data = 0xFFFFFFFF;
            bSignal2.repeatCount = 1;
            rule.bSequence.addSignal(bSignal2, 30);
            
            // 第三个B信号：第二次重复码 0xFFFFFFFF，间隔70ms
            UnifiedSignal bSignal3;
            bSignal3.protocol = NEC_PROTOCOL;
            bSignal3.useTiming = false;
            bSignal3.data = 0xFFFFFFFF;
            bSignal3.repeatCount = 1;
            rule.bSequence.addSignal(bSignal3, 70);
        } else {
            // 其他规则：单个B信号
            UnifiedSignal bSignal;
            bSignal.protocol = NEC_PROTOCOL;
            bSignal.useTiming = false;  // 协议信号
            bSignal.data = defaultRules[i].targetData;
            bSignal.repeatCount = 1;
            rule.bSequence.addSignal(bSignal, 0);
        }
        
        rule.bSequence.repeatCount = defaultRules[i].repeatCount;
        
        if (addRule(rule, false)) {
            successCount++;
            if (logger) {
                char buffer[64];
                snprintf(buffer, sizeof(buffer), "添加默认规则: %s", defaultRules[i].name);
                logger->log("system", buffer);
            }
        } else {
            char buffer[128];
            snprintf(buffer, sizeof(buffer), "添加默认规则失败: %s (可能已存在或数组已满)", defaultRules[i].name);
            if (logger) {
                logger->log("error", buffer);
            } else {
                Serial.printf("[IRMapper] %s\n", buffer);
            }
        }
    }
    
    // 检查实际添加的规则数量
    int actualCount = getRuleCount();
    if (actualCount != successCount) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "规则数量不一致 (添加成功: %d, 实际统计: %d)", successCount, actualCount);
        if (logger) {
            logger->log("warning", buffer);
        } else {
            Serial.printf("[IRMapper] %s\n", buffer);
        }
    }
    
    // 所有规则添加完成后，一次性保存
    if (successCount == 0) {
        if (logger) {
            logger->log("error", "没有成功添加任何默认规则");
        } else {
            Serial.println("[IRMapper] 没有成功添加任何默认规则");
        }
        return false;
    }
    
    bool saveSuccess = saveToFlash();
    if (saveSuccess) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "默认映射初始化完成，成功添加并保存 %d/%d 条规则", successCount, totalRules);
        if (logger) {
            logger->log("system", buffer);
        } else {
            Serial.printf("[IRMapper] %s\n", buffer);
        }
    } else {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "默认映射添加成功 (%d 条)，但保存失败", successCount);
        if (logger) {
            logger->log("error", buffer);
        } else {
            Serial.printf("[IRMapper] %s\n", buffer);
        }
    }
    
    return saveSuccess;
}

int IRMapper::findEmptySlot() const {
    // 统一数据源：遍历整个数组，找到第一个无效槽位
    for (int i = 0; i < MAX_MAP_RULES; i++) {
        if (!rules[i].valid) {
            return i;
        }
    }
    return -1;  // 数组已满
}

bool IRMapper::addRule(const IR_MapRule& rule, bool saveImmediately) {
    // 检查是否已存在相同的A信号（遍历整个数组）
    int existingIndex;
    if (findRuleIndex(rule.aSignal, existingIndex)) {
        return false;  // 已存在
    }
    
    // 找到第一个无效的槽位（统一数据源）
    int emptyIndex = findEmptySlot();
    if (emptyIndex == -1) {
        return false;  // 数组已满
    }
    
    // 在找到的空槽位插入规则
    rules[emptyIndex] = rule;
    rules[emptyIndex].id = nextId++;
    rules[emptyIndex].valid = true;
    
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
    
    // 只标记为无效，不移动数组元素（避免数组碎片化，统一数据源）
    rules[index].valid = false;
    
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
    // 统一数据源：遍历整个数组
    for (int i = 0; i < MAX_MAP_RULES; i++) {
        if (!rules[i].valid) continue;
        
        // 检查B信号序列
        for (int j = 0; j < rules[i].bSequence.signalCount; j++) {
            if (SignalUtils::signalsEqual(signal, rules[i].bSequence.signals[j])) {
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
    // 统计有效规则数量（确保准确性）
    // 遍历整个数组，与getAllRules保持一致
    int validCount = 0;
    for (int i = 0; i < MAX_MAP_RULES; i++) {
        if (rules[i].valid) {
            validCount++;
        }
    }
    return validCount;
}

int IRMapper::getAllRules(IR_MapRule* outputRules, int maxCount) {
    if (outputRules == nullptr || maxCount <= 0) {
        return 0;
    }
    
    // 只返回有效的规则（与saveToFlash保持一致）
    // 遍历整个数组，找到所有有效规则
    int outputCount = 0;
    for (int i = 0; i < MAX_MAP_RULES && outputCount < maxCount; i++) {
        if (rules[i].valid) {
            outputRules[outputCount++] = rules[i];
        }
    }
    
    return outputCount;  // 返回实际填充的规则数量
}

bool IRMapper::saveToFlash() {
    // 检查可用内存
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 4096) {  // 至少需要4KB空闲内存
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "内存不足，跳过保存 (空闲: %d bytes)", freeHeap);
        if (logger) {
            logger->log("error", buffer);
        } else {
            Serial.printf("[IRMapper] %s\n", buffer);
        }
        return false;
    }
    
    // 动态计算JSON大小：每条规则约200字节，加上JSON开销
    // 使用getRuleCount()统一数据源
    int validCount = getRuleCount();
    size_t jsonSize = (validCount * 200) + 2048;
    if (jsonSize < 4096) jsonSize = 4096;  // 最小4KB
    
    // 确保不超过可用内存
    if (jsonSize > freeHeap - 1024) {  // 保留1KB缓冲
        jsonSize = freeHeap - 1024;
        if (jsonSize < 1024) {
            if (logger) {
                logger->log("error", "内存不足，无法保存 (需要至少1KB)");
            } else {
                Serial.println("[IRMapper] 内存不足，无法保存 (需要至少1KB)");
            }
            return false;
        }
    }
    
    DynamicJsonDocument doc(jsonSize);
    JsonArray rulesArray = doc.to<JsonArray>();
    
    // 遍历整个数组，与getAllRules保持一致
    int savedCount = 0;
    for (int i = 0; i < MAX_MAP_RULES; i++) {
        if (!rules[i].valid) continue;
        
        JsonObject ruleObj = rulesArray.createNestedObject();
        // 使用统一方法，设置hexFormat=true，统一使用十六进制格式
        ruleToJsonObject(rules[i], ruleObj, true);
        savedCount++;
    }
    
    // 检查文档容量是否足够（ArduinoJson可能会在容量不足时截断）
    if (doc.overflowed()) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "JSON文档容量不足 (需要保存 %d 条规则，文档大小: %d bytes)", validCount, jsonSize);
        if (logger) {
            logger->log("error", buffer);
        } else {
            Serial.printf("[IRMapper] %s\n", buffer);
        }
        // 尝试使用更大的文档重新保存
        size_t largerSize = (validCount * 300) + 4096;  // 增加估算大小
        if (largerSize <= freeHeap - 1024 && largerSize <= 16384) {  // 限制最大16KB
            DynamicJsonDocument largerDoc(largerSize);
            JsonArray largerArray = largerDoc.to<JsonArray>();
            for (int i = 0; i < MAX_MAP_RULES; i++) {
                if (!rules[i].valid) continue;
                JsonObject ruleObj = largerArray.createNestedObject();
                ruleToJsonObject(rules[i], ruleObj, true);
            }
            String output;
            serializeJson(largerDoc, output);
            if (!largerDoc.overflowed() && Storage::saveFile(CONFIG_FILE_MAPS, output)) {
                if (logger) {
                    logger->log("system", "使用更大的文档成功保存所有规则");
                }
                return true;
            }
        }
        return false;
    }
    
    String output;
    serializeJson(doc, output);
    
    // 验证保存的规则数量
    if (savedCount != validCount) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "保存规则数量不匹配 (期望: %d, 实际: %d)", validCount, savedCount);
        if (logger) {
            logger->log("error", buffer);
        } else {
            Serial.printf("[IRMapper] %s\n", buffer);
        }
    }
    
    bool success = Storage::saveFile(CONFIG_FILE_MAPS, output);
    if (success && logger) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "成功保存 %d 条规则到Flash", savedCount);
        logger->log("system", buffer);
    }
    return success;
}

bool IRMapper::loadFromFlash() {
    // 使用文件流直接读取，避免String截断问题（参考Logger的实现）
    if (!LittleFS.exists(CONFIG_FILE_MAPS)) {
        if (logger) {
            logger->log("system", "Maps文件不存在，创建空文件");
        } else {
            Serial.println("[IRMapper] Maps文件不存在，创建空文件");
        }
        if (Storage::saveFile(CONFIG_FILE_MAPS, "[]")) {
            // 清空所有规则（统一数据源）
            for (int i = 0; i < MAX_MAP_RULES; i++) {
                rules[i].valid = false;
            }
            nextId = 1;
            return true;
        }
        return false;
    }
    
    File file = LittleFS.open(CONFIG_FILE_MAPS, "r");
    if (!file) {
        if (logger) {
            logger->log("error", "无法打开Maps文件");
        } else {
            Serial.println("[IRMapper] 无法打开Maps文件");
        }
        return false;
    }
    
    // 动态计算JSON大小：文件大小加上解析开销
    size_t fileSize = file.size();
    size_t jsonSize = fileSize + 1024;
    if (jsonSize < 4096) jsonSize = 4096;
    
    // 记录文件大小用于调试
    if (logger) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "读取Maps文件，大小: %d bytes", fileSize);
        logger->log("system", buffer);
    } else {
        Serial.printf("[IRMapper] 读取Maps文件，大小: %d bytes\n", fileSize);
    }
    
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    // 情况2：JSON解析失败（可能是旧格式或损坏）
    if (error) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "Maps文件JSON解析失败: %s (文件大小: %d bytes)", error.c_str(), fileSize);
        if (logger) {
            logger->log("error", buffer);
        } else {
            Serial.printf("[IRMapper] %s\n", buffer);
        }
        if (Storage::saveFile(CONFIG_FILE_MAPS, "[]")) {
            // 清空所有规则（统一数据源）
            for (int i = 0; i < MAX_MAP_RULES; i++) {
                rules[i].valid = false;
            }
            nextId = 1;
            return true;
        }
        return false;
    }
    
    // 检查文档是否溢出
    if (doc.overflowed()) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "JSON文档容量不足 (文件大小: %d bytes, 文档大小: %d bytes)", fileSize, jsonSize);
        if (logger) {
            logger->log("error", buffer);
        } else {
            Serial.printf("[IRMapper] %s\n", buffer);
        }
        // 尝试使用更大的文档重新读取
        file = LittleFS.open(CONFIG_FILE_MAPS, "r");
        if (file) {
            size_t largerSize = fileSize * 2 + 2048;
            if (largerSize <= 16384) {  // 限制最大16KB
                DynamicJsonDocument largerDoc(largerSize);
                DeserializationError largerError = deserializeJson(largerDoc, file);
                file.close();
                if (!largerError && !largerDoc.overflowed()) {
                    doc = largerDoc;
                    if (logger) {
                        logger->log("system", "使用更大的文档成功读取");
                    }
                } else {
                    if (logger) {
                        logger->log("error", "使用更大的文档仍然失败");
                    }
                    return false;
                }
            } else {
                file.close();
                return false;
            }
        }
    }
    
    JsonArray rulesArray = doc.as<JsonArray>();
    // 清空所有规则（统一数据源：遍历整个数组）
    for (int i = 0; i < MAX_MAP_RULES; i++) {
        rules[i].valid = false;
    }
    nextId = 1;
    bool hasValidRules = false;
    int originalRuleCount = rulesArray.size();
    int failedCount = 0;
    int loadedCount = 0;
    
    // 尝试加载规则（找到第一个无效槽位插入）
    for (JsonObject ruleObj : rulesArray) {
        if (loadedCount >= MAX_MAP_RULES) {
            if (logger) {
                char buffer[128];
                snprintf(buffer, sizeof(buffer), "达到最大规则数限制 (%d)，停止加载", MAX_MAP_RULES);
                logger->log("warning", buffer);
            }
            break;
        }
        
        // 找到第一个无效槽位（统一数据源）
        int emptyIndex = findEmptySlot();
        if (emptyIndex == -1) {
            // 数组已满，记录当前有效规则数量用于调试
            int currentValidCount = getRuleCount();
            char buffer[128];
            snprintf(buffer, sizeof(buffer), "数组已满，无法加载更多规则 (当前有效: %d, 已加载: %d)", currentValidCount, loadedCount);
            if (logger) {
                logger->log("warning", buffer);
            } else {
                Serial.printf("[IRMapper] %s\n", buffer);
            }
            break;
        }
        
        IR_MapRule& rule = rules[emptyIndex];
        // 使用统一方法解析（仅支持十六进制字符串格式）
        if (!jsonObjectToRule(ruleObj, rule)) {
            // 解析失败，可能是旧格式或格式错误
            failedCount++;
            char buffer[128];
            snprintf(buffer, sizeof(buffer), "规则解析失败 (已跳过 %d 条)", failedCount);
            if (logger) {
                logger->log("error", buffer);
            } else {
                Serial.printf("[IRMapper] %s\n", buffer);
            }
            continue;
        }
        
        rule.id = ruleObj["id"] | 0;
        if (rule.id >= nextId) {
            nextId = rule.id + 1;
        }
        
        rule.valid = true;
        loadedCount++;
        hasValidRules = true;
    }
    
    // 情况3：文件存在但所有规则都是旧格式（解析失败）
    if (!hasValidRules && originalRuleCount > 0) {
        if (logger) {
            logger->log("error", "检测到旧格式数据，重置为空文件");
        } else {
            Serial.println("[IRMapper] 检测到旧格式数据，重置为空文件");
        }
        if (Storage::saveFile(CONFIG_FILE_MAPS, "[]")) {
            // 清空所有规则（统一数据源）
            for (int i = 0; i < MAX_MAP_RULES; i++) {
                rules[i].valid = false;
            }
            nextId = 1;
            return true;
        }
        return false;
    }
    
    // 情况4：部分规则加载失败，如果失败率超过50%，重置文件
    if (originalRuleCount > 0 && failedCount > 0) {
        float failureRate = (float)failedCount / originalRuleCount;
        if (failureRate > 0.5f) {
            char buffer[128];
            snprintf(buffer, sizeof(buffer), "规则加载失败率过高 (%.1f%%)，重置文件", failureRate * 100);
            if (logger) {
                logger->log("error", buffer);
            } else {
                Serial.printf("[IRMapper] %s\n", buffer);
            }
            if (Storage::saveFile(CONFIG_FILE_MAPS, "[]")) {
                // 清空所有规则（统一数据源）
                for (int i = 0; i < MAX_MAP_RULES; i++) {
                    rules[i].valid = false;
                }
                nextId = 1;
                return true;
            }
            return false;
        } else {
            char buffer[128];
            snprintf(buffer, sizeof(buffer), "成功加载 %d/%d 条规则，跳过 %d 条无效规则", 
                    loadedCount, originalRuleCount, failedCount);
            if (logger) {
                logger->log("system", buffer);
            } else {
                Serial.printf("[IRMapper] %s\n", buffer);
            }
        }
    }
    
    // 最终验证：检查实际加载的规则数量
    int finalCount = getRuleCount();
    if (finalCount != loadedCount) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "警告：加载后规则数量不一致 (加载: %d, 统计: %d)", loadedCount, finalCount);
        if (logger) {
            logger->log("warning", buffer);
        } else {
            Serial.printf("[IRMapper] %s\n", buffer);
        }
    }
    // 注意：移除了"规则加载完成"的日志，因为：
    // 1. 如果加载成功，规则数量信息已经在"读取Maps文件"日志中体现
    // 2. 避免重复输出，减少日志噪音
    // 3. 如果需要，可以通过查询接口获取规则数量
    
    return true;
}

bool IRMapper::exportToJSON(String& json) {
    // 动态计算JSON大小：每条规则约200字节
    // 使用getRuleCount()统一数据源
    int validCount = getRuleCount();
    size_t jsonSize = (validCount * 200) + 2048;
    if (jsonSize < 4096) jsonSize = 4096;
    DynamicJsonDocument doc(jsonSize);
    JsonArray rulesArray = doc.to<JsonArray>();
    
    // 使用统一方法，设置hexFormat=true，统一使用十六进制格式（与saveToFlash一致）
    // 遍历整个数组，与getAllRules保持一致
    for (int i = 0; i < MAX_MAP_RULES; i++) {
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
    
    // 清空现有规则（统一数据源：遍历整个数组）
    for (int i = 0; i < MAX_MAP_RULES; i++) {
        rules[i].valid = false;
    }
    nextId = 1;
    
    JsonArray rulesArray = doc.as<JsonArray>();
    int loadedCount = 0;
    for (JsonObject ruleObj : rulesArray) {
        if (loadedCount >= MAX_MAP_RULES) break;
        
        // 找到第一个无效槽位（统一数据源）
        int emptyIndex = findEmptySlot();
        if (emptyIndex == -1) break;  // 数组已满
        
        IR_MapRule rule;
        // 使用统一方法解析（仅支持十六进制字符串格式）
        if (!jsonObjectToRule(ruleObj, rule)) {
            continue;  // 解析失败，跳过这条规则
        }
        
        rule.id = ruleObj["id"] | 0;
        rules[emptyIndex] = rule;
        rules[emptyIndex].valid = true;
        loadedCount++;
        if (rule.id >= nextId) {
            nextId = rule.id + 1;
        }
    }
    
    return saveToFlash();
}

bool IRMapper::findRuleIndex(const UnifiedSignal& aSignal, int& index) {
    // 统一数据源：遍历整个数组
    for (int i = 0; i < MAX_MAP_RULES; i++) {
        if (!rules[i].valid) continue;
        
        if (SignalUtils::signalsEqual(aSignal, rules[i].aSignal)) {
            index = i;
            return true;
        }
    }
    
    return false;
}

bool IRMapper::findRuleIndexById(uint8_t id, int& index) {
    // 统一数据源：遍历整个数组
    for (int i = 0; i < MAX_MAP_RULES; i++) {
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
    
    // B信号序列（使用统一序列序列化方法）
    JsonObject bSequence = obj.createNestedObject("bSequence");
    SignalUtils::sequenceToJSON(rule.bSequence, bSequence, hexFormat);
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
    
    // B信号序列（使用统一序列反序列化方法）
    // 兼容旧格式：如果存在 bSignals 数组，转换为新格式
    if (obj.containsKey("bSequence")) {
        JsonObject bSequence = obj["bSequence"];
        if (bSequence.isNull()) {
            return false;
        }
        if (!SignalUtils::sequenceFromJSON(bSequence, rule.bSequence)) {
            return false;
        }
    } else if (obj.containsKey("bSignals")) {
        // 兼容旧格式：从 bSignals 数组和 intervals 数组构建序列
        JsonArray bSignals = obj["bSignals"];
        if (bSignals.isNull() || bSignals.size() == 0) {
            return false;
        }
        
        rule.bSequence.clear();
        JsonArray intervals = obj["intervals"];
        int intervalIndex = 0;
        
        for (JsonObject bSignal : bSignals) {
            if (rule.bSequence.signalCount >= MAX_B_SIGNALS_PER_RULE) break;
            
            UnifiedSignal signal;
            if (!SignalUtils::signalFromJSON(bSignal, signal)) {
                return false;
            }
            
            uint16_t interval = 0;
            if (intervalIndex < intervals.size() && intervalIndex < rule.bSequence.signalCount) {
                interval = intervals[intervalIndex];
            }
            
            if (!rule.bSequence.addSignal(signal, interval)) {
                return false;
            }
            intervalIndex++;
        }
        
        if (rule.bSequence.signalCount == 0) {
            return false;
        }
        
        rule.bSequence.repeatCount = obj["repeatCount"] | 1;
        if (rule.bSequence.repeatCount == 0) {
            rule.bSequence.repeatCount = 1;
        }
    } else {
        return false;  // 缺少bSequence或bSignals字段
    }
    
    rule.valid = true;
    return true;
}
