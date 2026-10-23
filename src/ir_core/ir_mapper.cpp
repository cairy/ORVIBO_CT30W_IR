#include "ir_mapper.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <string.h>
#include "utils/hex_utils.h"
#include "utils/logger.h"
#include "utils/timing_scratch.h"
#include "unified_signal.h"
#include "ir_core/ir_learner.h"

// a/b 字符串格式解析用的时序暂存：复用全局共享缓冲（原先单独常驻 1,600 B）
// 见 src/utils/timing_scratch.h —— 只在 jsonObjectToRule 内使用，且不跨调用持有。

IRMapper::IRMapper() : ruleCount(0), nextId(1), logger(nullptr), loadFailed(false) {
    for (int i = 0; i < MAX_MAP_RULES; i++) {
        rules[i].valid = false;
        rules[i].name[0] = '\0';
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
            Serial.printf_P(PSTR("[IRMapper] %s\n"), buffer);
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
    
    // 顺序：电源、AV/TV、音量+、音量-、静音（空表时 id 从 1 开始依次分配）
    DefaultRule defaultRules[] = {
        {SOURCE_POWER, TARGET_POWER, 1, "电源"},
        {SOURCE_AVTV, TARGET_AVTV, 1, "AV/TV"},
        {SOURCE_VOLUP, TARGET_VOLUP, 1, "音量+"},
        {SOURCE_VOLDOWN, TARGET_VOLDOWN, 1, "音量-"},
        {SOURCE_MUTE, TARGET_MUTE, 1, "静音"}
    };
    
    // 初始化基础规则结构
    rule.aSignal.protocol = NEC_PROTOCOL;
    rule.aSignal.useTiming = false;  // 协议信号
    rule.aSignal.repeatCount = 1;
    
    rule.bSequence.clear();
    rule.valid = true;
    
    // 批量添加规则，不立即保存（节省内存）
    int successCount = 0;
    int totalRules = sizeof(defaultRules) / sizeof(defaultRules[0]);
    
    for (int i = 0; i < totalRules; i++) {
        rule.aSignal.data = defaultRules[i].sourceData;
        strncpy(rule.name, defaultRules[i].name, MAX_COMMAND_NAME_LEN - 1);
        rule.name[MAX_COMMAND_NAME_LEN - 1] = '\0';

        // 构建B信号序列
        rule.bSequence.clear();
        
        // 特殊处理电源规则：包含三个B信号（完整信号 + 2次重复码）
        // 参考 ir-converter-finally.ino：完整信号 + 延时30ms + 重复码 + 延时70ms + 重复码
        // 第一个完整 NEC 前的首间隔（与全局转发前延时叠加，可按现场调整）
        if (defaultRules[i].sourceData == SOURCE_POWER) {
            // 第一个B信号：完整信号 0xFDB04F，第一个信号前间隔 120ms
            UnifiedSignal bSignal1;
            bSignal1.protocol = NEC_PROTOCOL;
            bSignal1.useTiming = false;
            bSignal1.data = TARGET_POWER;  // 0xFDB04F
            bSignal1.repeatCount = 1;
            rule.bSequence.addSignal(bSignal1, 120, true);
            
            // 第二个B信号：第一次重复码 0xFFFFFFFF，间隔30ms
            UnifiedSignal bSignal2;
            bSignal2.protocol = NEC_PROTOCOL;
            bSignal2.useTiming = false;
            bSignal2.data = 0xFFFFFFFF;
            bSignal2.repeatCount = 1;
            rule.bSequence.addSignal(bSignal2, 30, true);
            
            // 第三个B信号：第二次重复码 0xFFFFFFFF，间隔70ms
            UnifiedSignal bSignal3;
            bSignal3.protocol = NEC_PROTOCOL;
            bSignal3.useTiming = false;
            bSignal3.data = 0xFFFFFFFF;
            bSignal3.repeatCount = 1;
            rule.bSequence.addSignal(bSignal3, 70, true);
        } else if (defaultRules[i].sourceData == SOURCE_AVTV) {
            // AV/TV规则：单个B信号，第一个信号前间隔30ms
            UnifiedSignal bSignal;
            bSignal.protocol = NEC_PROTOCOL;
            bSignal.useTiming = false;  // 协议信号
            bSignal.data = defaultRules[i].targetData;
            bSignal.repeatCount = 1;
            rule.bSequence.addSignal(bSignal, 30, true);
        } else {
            // 其他规则：单个B信号
            UnifiedSignal bSignal;
            bSignal.protocol = NEC_PROTOCOL;
            bSignal.useTiming = false;  // 协议信号
            bSignal.data = defaultRules[i].targetData;
            bSignal.repeatCount = 1;
            rule.bSequence.addSignal(bSignal, 0);
        }
        
        // 注意：序列不再包含repeatCount，重复次数由外部控制
        
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
                Serial.printf_P(PSTR("[IRMapper] %s\n"), buffer);
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
            Serial.printf_P(PSTR("[IRMapper] %s\n"), buffer);
        }
    }
    
    // 所有规则添加完成后，一次性保存
    if (successCount == 0) {
        if (logger) {
            logger->log("error", "没有成功添加任何默认规则");
        } else {
            Serial.println(F("[IRMapper] 没有成功添加任何默认规则"));
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
            Serial.printf_P(PSTR("[IRMapper] %s\n"), buffer);
        }
    } else {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "默认映射添加成功 (%d 条)，但保存失败", successCount);
        if (logger) {
            logger->log("error", buffer);
        } else {
            Serial.printf_P(PSTR("[IRMapper] %s\n"), buffer);
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

// 取一个可用的规则 ID：在 1..255 里循环找第一个**当前没被占用**的值。
// 为什么不用 `nextId++`：nextId 是 uint8_t，加到 255 会回绕成 0；而 id == 0 在本模块
// 各处都被当作"非法/无效"（deleteRule / handleUpdateMap 的 `if (id != 0)`），
// 于是回绕后新分配的规则会**永远删不掉**。这里显式跳过 0，并跳过已占用的号，
// 保证恒有合法且唯一的 ID（规则上限 MAX_MAP_RULES 远小于 255，不会找不到）。
uint8_t IRMapper::allocateId() {
    // nextId 可能是 0：loadFromFlash 里有 `nextId = rule.id + 1`，而 rule.id 是 uint8_t，
    // 取到 255 时 +1 就溢出成 0。先把它规范回 1..255，否则下面的取模会算出 0。
    int start = (int)nextId;
    if (start < 1 || start > 255) {
        start = 1;
    }
    for (int step = 0; step < 255; step++) {
        int candidate = (start - 1 + step) % 255 + 1;  // 恒在 1..255，永不为 0
        bool used = false;
        for (int i = 0; i < MAX_MAP_RULES; i++) {
            if (rules[i].valid && rules[i].id == (uint8_t)candidate) {
                used = true;
                break;
            }
        }
        if (!used) {
            nextId = (uint8_t)candidate;
            return (uint8_t)candidate;
        }
    }
    return 1;  // 理论不可达（20 条规则用不满 255 个号）
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
    
    // 在找到的空槽位插入规则。ID 由 allocateId() 取（跳过 0、跳过已占用的），
    // 不再用 `nextId++` —— uint8_t 加到 255 会回绕成 0，而 0 在本模块是非法值。
    rules[emptyIndex] = rule;
    rules[emptyIndex].id = allocateId();
    rules[emptyIndex].valid = true;
    
    if (saveImmediately && !saveToFlash()) {
        // 落盘失败必须让调用方知道：以前这里丢弃返回值照样 return true，
        // 于是"文件没写成"也报成功，用户重启才发现白改（v1.3.18 已在批量路径
        // 修过一次，单条路径漏了）。
        return false;
    }
    return true;
}

bool IRMapper::updateRule(uint8_t id, const IR_MapRule& rule, bool saveImmediately) {
    int index;
    if (!findRuleIndexById(id, index)) {
        return false;
    }
    
    rules[index] = rule;
    rules[index].id = id;  // 保持原有ID
    rules[index].valid = true;
    
    if (saveImmediately && !saveToFlash()) {
        return false;   // 同上：落盘失败不能报成功
    }
    return true;
}

bool IRMapper::deleteRule(uint8_t id, bool saveImmediately) {
    int index;
    if (!findRuleIndexById(id, index)) {
        return false;
    }
    
    // 只标记为无效，不移动数组元素（避免数组碎片化，统一数据源）
    rules[index].valid = false;
    rules[index].name[0] = '\0';

    if (saveImmediately && !saveToFlash()) {
        return false;   // 同上：落盘失败不能报成功
    }
    return true;
}

IR_MapRule* IRMapper::findRule(const UnifiedSignal& aSignal) {
    int index;
    if (findRuleIndex(aSignal, index)) {
        return &rules[index];
    }
    return nullptr;
}

IR_MapRule* IRMapper::findRuleById(uint8_t id) {
    int index;
    if (findRuleIndexById(id, index)) {
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

int IRMapper::collectIds(uint8_t* out, int maxCount) const {
    if (out == nullptr || maxCount <= 0) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < MAX_MAP_RULES && n < maxCount; i++) {
        if (rules[i].valid) {
            out[n++] = rules[i].id;
        }
    }
    return n;
}

// 时序码信号在 JSON 里额外占多少字节（Base64 会被复制进 ArduinoJson 的 string pool）
static inline size_t timingJsonCost(const UnifiedSignal& s) {
    return s.useTiming ? ((size_t)s.timingBase64.length() + 16) : 0;
}

// 保存映射规则到 Flash。
//
// ⚠️ v1.3.17 重写，根因与 Discovery 那次「恒久发不出去」是同一类：
// 旧实现先把**全部**规则塞进一个 DynamicJsonDocument（每条按 200 B 预留、下限 4096 B）
// 再整体序列化，并且开头有一条绝对的 `freeHeap < 4096 ⇒ 跳过保存`。而本机**配了 MQTT
// 之后空闲堆只有 3~5 KB** —— 那条门槛让"规则改动"能进 RAM 却永远不落盘：HA 里删改规则
// 后 rule_count 当场就变，**重启又变回去**，只在日志里留一行 error。
// 现在改成**边遍历边写**：峰值内存只取决于单条规则，与规则总条数无关。
//
// 同时改用「临时文件 + rename」：旧实现一 open("w") 就把原文件清空，写到一半失败
// 只剩半截 JSON；现在任何一步失败都保留原文件。
bool IRMapper::saveToFlash() {
    int validCount = getRuleCount();

    // 单条规则需要的 JSON DOM：id + name + aSignal + bSequence(signals[] + intervals[]
    // + timestamp/matched)。每个信号对象约 6 个字段 ≈ 170 B，按 224 B/信号预留 + 256 B 骨架。
    //
    // ⚠️ 时序码信号要额外算上 Base64：ruleToJsonObject 写的是
    // `obj["timingBase64"] = signal.timingBase64`，ArduinoJson 会把整串**复制进
    // string pool**。200 个时序点 ≈ 400 字节 → 约 534 字符，按 224 B/信号估算会
    // 严重低估 ⇒ ruleDoc 溢出 ⇒ 走到下面 failed=true ⇒ **整份规则一条都不存**，
    // 再叠加"落盘返回值被忽略"⇒ 用户在面板上完全无感。这里按实际长度累加。
    size_t docSize = 256;   // 骨架下限
    for (int i = 0; i < MAX_MAP_RULES; i++) {
        if (!rules[i].valid) continue;
        size_t need = 256 + timingJsonCost(rules[i].aSignal);
        for (int j = 0; j < rules[i].bSequence.signalCount; j++) {
            need += 224 + timingJsonCost(rules[i].bSequence.signals[j]);
        }
        if (need > docSize) docSize = need;   // 取最大的单条（DOM 是逐条复用的）
    }
    if (docSize < 768) docSize = 768;

    // 堆不够就别开文件（原文件保持不动）。留 1 KB 给 LittleFS 的写缓冲。
    size_t freeHeap = ESP.getFreeHeap();
    if (docSize + 1024 > freeHeap) {
        char buffer[160];
        snprintf(buffer, sizeof(buffer), "内存不足，规则未保存 (需要 %u, 空闲: %u bytes)",
                 (unsigned)(docSize + 1024), (unsigned)freeHeap);
        if (logger) {
            logger->log("error", buffer);
        } else {
            Serial.printf_P(PSTR("[IRMapper] %s\n"), buffer);
        }
        return false;
    }

    File file = LittleFS.open(CONFIG_FILE_MAPS_TMP, "w");
    if (!file) {
        if (logger) {
            logger->log("error", "打开规则临时文件失败，规则未保存");
        } else {
            Serial.println(F("[IRMapper] 打开规则临时文件失败"));
        }
        return false;
    }

    DynamicJsonDocument ruleDoc(docSize);
    file.print('[');
    int savedCount = 0;
    bool failed = false;
    for (int i = 0; i < MAX_MAP_RULES; i++) {
        if (!rules[i].valid) continue;

        ruleDoc.clear();
        JsonObject ruleObj = ruleDoc.to<JsonObject>();
        ruleToJsonObject(rules[i], ruleObj, true);  // true = hex 格式
        if (ruleDoc.overflowed()) {
            failed = true;
            break;
        }
        if (savedCount > 0) {
            file.print(',');
        }
        serializeJson(ruleDoc, file);
        savedCount++;
    }
    if (!failed) {
        file.print(']');
    }
    file.close();

    // 条数不符也按失败处理：宁可保留上一份完整规则，也不落半截
    if (failed || savedCount != validCount) {
        LittleFS.remove(CONFIG_FILE_MAPS_TMP);
        char buffer[160];
        if (failed) {
            snprintf(buffer, sizeof(buffer),
                     "规则未保存：单条规则 JSON 超出 %u bytes 缓冲（空闲: %u bytes）",
                     (unsigned)docSize, (unsigned)ESP.getFreeHeap());
        } else {
            snprintf(buffer, sizeof(buffer),
                     "规则未保存：条数不匹配（期望 %d，实际 %d）", validCount, savedCount);
        }
        if (logger) {
            logger->log("error", buffer);
        } else {
            Serial.printf_P(PSTR("[IRMapper] %s\n"), buffer);
        }
        return false;
    }

    // 原子替换：删旧 → 改名。两步之间掉电，最坏是规则文件缺失（开机走默认规则），
    // 但绝不会留下半截 JSON。
    LittleFS.remove(CONFIG_FILE_MAPS);
    if (!LittleFS.rename(CONFIG_FILE_MAPS_TMP, CONFIG_FILE_MAPS)) {
        LittleFS.remove(CONFIG_FILE_MAPS_TMP);
        if (logger) {
            logger->log("error", "规则文件替换失败，规则未保存");
        } else {
            Serial.println(F("[IRMapper] 规则文件替换失败"));
        }
        return false;
    }

    Serial.printf_P(PSTR("[IRMapper] 成功保存 %d 条规则到Flash\n"), savedCount);
    if (logger) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "成功保存 %d 条规则到Flash", savedCount);
        logger->log("system", buffer);
    }
    return true;
}

bool IRMapper::loadFromFlash() {
    loadFailed = false;
    // 使用文件流直接读取，避免String截断问题（参考Logger的实现）
    if (!LittleFS.exists(CONFIG_FILE_MAPS)) {
        if (logger) {
            logger->log("system", "Maps文件不存在，创建空文件");
        } else {
            Serial.println(F("[IRMapper] Maps文件不存在，创建空文件"));
        }
        if (Storage::saveFile(CONFIG_FILE_MAPS, "[]")) {
            // 清空所有规则（统一数据源）
            for (int i = 0; i < MAX_MAP_RULES; i++) {
                rules[i].valid = false;
            }
            nextId = 1;
            return true;
        }
        loadFailed = true;
        return false;
    }
    
    File file = LittleFS.open(CONFIG_FILE_MAPS, "r");
    if (!file) {
        if (logger) {
            logger->log("error", "无法打开Maps文件");
        } else {
            Serial.println(F("[IRMapper] 无法打开Maps文件"));
        }
        loadFailed = true;
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
        Serial.printf_P(PSTR("[IRMapper] 读取Maps文件，大小: %d bytes\n"), fileSize);
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
            Serial.printf_P(PSTR("[IRMapper] %s\n"), buffer);
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
            Serial.printf_P(PSTR("[IRMapper] %s\n"), buffer);
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
                        logger->log("error", "使用更大的文档仍然失败（保留原文件，不写默认规则）");
                    }
                    loadFailed = true;
                    return false;
                }
            } else {
                file.close();
                if (logger) {
                    logger->log("error", "Maps 文件过大，本次读不出来（保留原文件，不写默认规则）");
                }
                loadFailed = true;
                return false;
            }
        } else {
            // 重新打开文件都失败 —— 同样是"读不出来"，不能当成"规则为空"
            loadFailed = true;
            return false;
        }
    }
    
    if (!doc.is<JsonArray>()) {
        // 根不是数组 = 文件已经坏了。以前会走到 as<JsonArray>() 拿到一个空数组，
        // 于是"清空内存规则 + return true"，随后被 initDefaultMappings() 用 5 条
        // 默认规则覆盖文件 ⇒ 用户规则不可逆丢失。现在明确报失败并保留原文件。
        if (logger) {
            logger->log("error", "Maps 文件根节点不是数组，保留原文件不覆盖");
        } else {
            Serial.println(F("[IRMapper] Maps 文件根节点不是数组"));
        }
        loadFailed = true;
        return false;
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
                Serial.printf_P(PSTR("[IRMapper] %s\n"), buffer);
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
                Serial.printf_P(PSTR("[IRMapper] %s\n"), buffer);
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
            Serial.println(F("[IRMapper] 检测到旧格式数据，重置为空文件"));
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
                Serial.printf_P(PSTR("[IRMapper] %s\n"), buffer);
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
                Serial.printf_P(PSTR("[IRMapper] %s\n"), buffer);
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
            Serial.printf_P(PSTR("[IRMapper] %s\n"), buffer);
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
    if (rule.name[0] != '\0') {
        // 验证name字段是否有效，避免保存乱码
        bool isValid = true;
        const char* p = rule.name;
        int len = 0;
        while (*p != '\0' && len < MAX_COMMAND_NAME_LEN) {
            // 检查明显的JSON结构片段（可能是之前保存时的错误）
            if ((*p == ':' && (p[1] == '[' || p[1] == '{' || (p[1] >= '0' && p[1] <= '9'))) ||
                (*p == ',' && p[1] == '"') ||
                (*p == '"' && (p == rule.name || p[1] == ':' || p[1] == ',')) ||
                (*p == '{' || *p == '}' || *p == '[' || *p == ']')) {
                isValid = false;
                break;
            }
            p++;
            len++;
        }
        // 只保存有效的name字段
        if (isValid && len > 0) {
            obj["name"] = rule.name;
        }
    }
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
    rule.name[0] = '\0';
    if (obj.containsKey("name") && obj["name"].is<const char*>()) {
        const char* s = obj["name"].as<const char*>();
        if (s) {
            // 验证字符串是否有效：检查是否包含明显的JSON结构片段（可能是解析错误导致的乱码）
            // 检查常见的JSON结构片段，如 ":["、":{、":0、",\"等
            bool isValid = true;
            const char* p = s;
            int len = 0;
            while (*p != '\0' && len < MAX_COMMAND_NAME_LEN) {
                // 检查明显的JSON结构片段
                if ((*p == ':' && (p[1] == '[' || p[1] == '{' || (p[1] >= '0' && p[1] <= '9'))) ||
                    (*p == ',' && p[1] == '"') ||
                    (*p == '"' && (p == s || p[1] == ':' || p[1] == ',')) ||
                    (*p == '{' || *p == '}' || *p == '[' || *p == ']')) {
                    isValid = false;
                    break;
                }
                p++;
                len++;
            }
            
            // 如果字符串有效且长度合理，才复制
            if (isValid && len > 0 && len < MAX_COMMAND_NAME_LEN) {
                strncpy(rule.name, s, MAX_COMMAND_NAME_LEN - 1);
                rule.name[MAX_COMMAND_NAME_LEN - 1] = '\0';
            }
        }
    }
    // A信号：优先对象格式，否则支持字符串格式 "a"
    JsonObject aSignal = obj["aSignal"];
    if (!aSignal.isNull()) {
        if (!SignalUtils::signalFromJSON(aSignal, rule.aSignal)) {
            return false;
        }
    } else if (obj.containsKey("a") && obj["a"].is<const char*>()) {
        const char* aStr = obj["a"].as<const char*>();
        if (!aStr || aStr[0] == '\0') {
            return false;
        }
        SignalSequence tempSeq;
        if (!IRLearner::parseStringSequence(String(aStr), tempSeq, timingScratch(), TIMING_SCRATCH_CAPACITY)) {
            return false;
        }
        if (tempSeq.signalCount != 1) {
            return false;  // A 信号必须为单个信号
        }
        rule.aSignal = tempSeq.signals[0];
    } else {
        return false;  // 缺少 aSignal 或 a 字段
    }

    // B信号序列：优先对象格式（bSequence / bSignals），否则支持字符串格式 "b"
    rule.bSequence.clear();
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
    } else if (obj.containsKey("b") && obj["b"].is<const char*>()) {
        const char* bStr = obj["b"].as<const char*>();
        if (!bStr || bStr[0] == '\0') {
            return false;
        }
        if (!IRLearner::parseStringSequence(String(bStr), rule.bSequence, timingScratch(), TIMING_SCRATCH_CAPACITY)) {
            return false;
        }
    } else {
        return false;  // 缺少 bSequence、bSignals 或 b 字段
    }
    
    rule.valid = true;
    return true;
}

