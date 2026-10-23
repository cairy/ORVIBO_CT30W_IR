#include "api_handlers.h"
#include "ir_core/unified_signal.h"
#include "config/constants.h"
#include <LittleFS.h>
#include <new>

String APIHandlers::handleQueryMaps(IRMapper& mapper, Logger* logger) {
    // 优化：直接读取文件内容，避免重新构建JSON文档（节省大量内存）
    // 文件已经是以hex格式保存的，格式正确，可以直接返回
    if (LittleFS.exists(CONFIG_FILE_MAPS)) {
        File file = LittleFS.open(CONFIG_FILE_MAPS, "r");
        if (file) {
            size_t fileSize = file.size();
            // 检查文件大小是否合理
            if (fileSize > 0 && fileSize < 32768) {  // 最大32KB
                String content = file.readString();
                file.close();
                
                // 验证内容是否为有效的JSON数组（避免 trim 产生字符串副本）
                const char* p = content.c_str();
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
                if (*p == '[') {
                    // 检查消息大小，如果接近或超过MQTT缓冲区限制，记录警告
                    size_t estimatedMqttSize = content.length() + 256;  // 估算包装开销
                    if (logger != nullptr && estimatedMqttSize > MQTT_BUFFER_SIZE * 0.8) {
                        char warnMsg[256];
                        snprintf(warnMsg, sizeof(warnMsg), 
                                 "[API: /api/maps] 文件大小: %d bytes, 估算MQTT消息大小: %d bytes (缓冲区: %d bytes, %.1f%%)", 
                                 content.length(), estimatedMqttSize, MQTT_BUFFER_SIZE, 
                                 (estimatedMqttSize * 100.0f / MQTT_BUFFER_SIZE));
                        logger->log(estimatedMqttSize > MQTT_BUFFER_SIZE ? "error" : "warning", warnMsg);
                    }
                    return content;
                }
            }
            file.close();
        }
    }
    
    // 如果文件不存在或读取失败，使用传统方法重新构建
    int count = mapper.getRuleCount();
    if (count == 0) {
        // 如果没有规则，返回空数组
        return "[]";
    }
    
    size_t jsonSize = MemoryMonitor::calculateRulesJsonSize(count);
    if (!MemoryMonitor::checkMemory(jsonSize + 1024)) {
        return "";  // 内存不足，返回空字符串
    }
    
    // 使用堆分配，避免栈溢出
    IR_MapRule* rules = new (std::nothrow) IR_MapRule[count];
    if (rules == nullptr) {
        return "";  // 分配失败
    }
    
    // 获取所有规则，返回实际填充的数量
    int actualCount = mapper.getAllRules(rules, count);
    if (actualCount == 0) {
        delete[] rules;
        return "[]";
    }
    
    // 构建JSON（只返回data部分）
    // 使用保守估算：每条规则约2000字节
    const size_t MAX_DOC_SIZE = 16384;  // 最大文档大小16KB
    size_t totalSize = 512 + (actualCount * 2000);
    if (totalSize < 4096) totalSize = 4096;
    if (totalSize > MAX_DOC_SIZE) totalSize = MAX_DOC_SIZE;
    
    DynamicJsonDocument doc(totalSize);
    JsonArray rulesArray = doc.to<JsonArray>();
    
    // 使用统一的转换方法
    for (int i = 0; i < actualCount; i++) {
        JsonObject ruleObj = rulesArray.createNestedObject();
        IRMapper::ruleToJsonObject(rules[i], ruleObj, true);  // true表示使用hex格式
    }
    
    // 检查文档是否溢出
    if (doc.overflowed()) {
        delete[] rules;
        if (logger != nullptr) {
            logger->log("error", "[API: /api/maps] JSON文档溢出，无法构建规则");
        }
        return "";
    }
    
    // 序列化
    String output;
    serializeJson(doc, output);
    delete[] rules;

    if (doc.overflowed()) {
        if (logger != nullptr) {
            logger->log("error", "[API: /api/maps] JSON文档溢出，无法序列化规则");
        }
        return "";
    }
    
    return output;
}

String APIHandlers::handleUpdateMap(IRMapper& mapper, const String& json, bool& success) {
    success = false;
    
    size_t jsonSize = json.length() + 1024;
    if (jsonSize > 6144) jsonSize = 6144;
    if (jsonSize < 512) jsonSize = 512;
    
    // 门槛随 payload 缩放（v1.3.17）：原先是 jsonSize + 2048，于是哪怕只有几十字节的
    // update_map 也被要求 ~3.2 KB 连续块 —— 配了 MQTT（空闲堆 ~4 KB）时实测直接回
    // "Low Memory"。现在只留一个与文档大小相称的安全余量。
    // 注：MQTT 侧那条路径同时改了 processCommand —— 它以前把解析 doc 一直留到分发之后，
    // 与本函数要的分配叠在一起，是同一症状的另一半原因。
    if (!MemoryMonitor::checkMemory(jsonSize + 1024)) {
        return "Low Memory";
    }
    
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, json);
    
    if (error) {
        return "Invalid JSON";
    }
    
    // ── 可选语义（v1.3.16）：mode = "append" / "delete" ──────────────────
    // 不带 mode 时保持旧行为（见下方"默认语义"）。加 mode 是为了让 HA 这类程序化
    // 客户端既能"只增不减"，也能"按 id 删一条"，不必为了删一条把全量规则重发
    //（旧批量语义是"先删光再写"，漏发一条就会误删）。
    String mode = doc["mode"] | "";
    mode.toLowerCase();
    
    if (mode == "append") {
        if (!doc.containsKey("rules")) {
            return "append mode requires 'rules' array";
        }
        JsonArray rulesArray = doc["rules"];
        int added = 0, skipped = 0;
        for (JsonObject ruleObj : rulesArray) {
            IR_MapRule rule;
            // 重复 A 信号 / 数组已满 / 格式非法的条目计入 skipped，其余正常追加。
            // ⚠️ 传 false：只在内存里改，循环结束后**统一落盘一次**。
            // 否则每 addRule 一条就是一次全量文件重写（见头文件里的说明）。
            if (IRMapper::jsonObjectToRule(ruleObj, rule) && mapper.addRule(rule, false)) {
                added++;
            } else {
                skipped++;
            }
        }
        // 没有实际新增就不落盘（省掉一次无谓的 flash 写）
        bool saved = (added == 0) ? true : mapper.saveToFlash();
        char msg[96];
        if (saved) {
            snprintf(msg, sizeof(msg), "Rules appended, added: %d, skipped: %d", added, skipped);
        } else {
            // 内存里已生效、但没能写进 Flash ⇒ 必须报出来，否则重启后静默丢规则
            snprintf(msg, sizeof(msg),
                     "Rules appended, added: %d, skipped: %d, flash write failed", added, skipped);
        }
        success = (added > 0) && saved;
        return String(msg);
    }
    
    if (mode == "delete") {
        int deleted = 0, missing = 0;
        if (doc.containsKey("ids")) {
            for (JsonVariant v : doc["ids"].as<JsonArray>()) {
                uint8_t id = (uint8_t)(v.as<int>());
                // 传 false：只改内存，循环后统一落盘一次（同 append 分支的理由）
                if (id != 0 && mapper.deleteRule(id, false)) deleted++; else missing++;
            }
        } else if (doc.containsKey("id")) {
            uint8_t id = (uint8_t)(doc["id"].as<int>());
            if (id != 0 && mapper.deleteRule(id, false)) deleted++; else missing++;
        } else {
            return "delete mode requires 'id' or 'ids'";
        }
        // 同 append：循环里只改内存，统一落盘一次
        bool saved = (deleted == 0) ? true : mapper.saveToFlash();
        char msg[96];
        if (saved) {
            snprintf(msg, sizeof(msg), "Rules deleted: %d, not found: %d", deleted, missing);
        } else {
            snprintf(msg, sizeof(msg),
                     "Rules deleted: %d, not found: %d, flash write failed", deleted, missing);
        }
        success = (deleted > 0) && saved;
        return String(msg);
    }
    
    // ── 默认语义（向后兼容）────────────────────────────────────────────
    // 检查是否是批量更新（包含rules数组）
    if (doc.containsKey("rules")) {
        // 批量更新：清空现有规则，添加新规则
        JsonArray rulesArray = doc["rules"];

        if (rulesArray.size() > MAX_MAP_RULES) {
            // 直接拒绝，而不是"删光了再发现塞不下" —— 后者会把用户的老规则全弄丢
            char tooMany[80];
            snprintf(tooMany, sizeof(tooMany), "rules array too large (max %d)", MAX_MAP_RULES);
            return String(tooMany);
        }

        // ── 先校验，后动手 ────────────────────────────────────────────────
        // 全量语义是"先删光再加"，中间**没有回滚快照**。只要有一条解析不了，
        // 老规则就已经没了、新的也补不上。所以先空跑一遍确认每条都能解析：
        // 解析是纯 CPU（不新增堆峰值），而这个接口是低频的配置操作，多跑一遍
        // 远比"悄悄清空用户 20 条规则"划算。
        int parsedCount = 0;
        for (JsonObject ruleObj : rulesArray) {
            IR_MapRule probe;
            if (IRMapper::jsonObjectToRule(ruleObj, probe)) {
                parsedCount++;
            }
        }
        if (parsedCount != (int)rulesArray.size()) {
            char bad[80];
            snprintf(bad, sizeof(bad), "invalid rule format (%d of %d parsed)",
                     parsedCount, (int)rulesArray.size());
            return String(bad);
        }

        // ── 先全删 ────────────────────────────────────────────────────────
        // ⚠️ 以前这里是 `new (std::nothrow) IR_MapRule[count]` 抄一份现有规则。
        // 20 条 ≈ 几 KB，在配了 MQTT（空闲只剩 3~5 KB）时几乎必然返回 nullptr，
        // 而失败分支会**静默跳过删除**、直接往下走追加：旧规则没删、新规则又和
        // 它们撞 A 信号被拒 ⇒ 既没删也没加，响应却仍是 success=true。
        // "先全删"只需要 id，20 条 = 20 字节，走调用方的栈数组，不可能失败。
        uint8_t oldIds[MAX_MAP_RULES];
        int oldCount = mapper.collectIds(oldIds, MAX_MAP_RULES);
        for (int i = 0; i < oldCount; i++) {
            mapper.deleteRule(oldIds[i], false);
        }

        // ── 再全加 ────────────────────────────────────────────────────────
        int successCount = 0;
        for (JsonObject ruleObj : rulesArray) {
            IR_MapRule rule;
            if (IRMapper::jsonObjectToRule(ruleObj, rule)) {
                if (mapper.addRule(rule, false)) {
                    successCount++;
                }
            }
        }

        // 全量替换只落盘一次（v1.3.18）：原来是"逐条删各写一次 + 逐条加各写一次"，
        // 面板上保存 5 条规则 = 10 次全量文件重写，实测 1.937s。
        bool saved = mapper.saveToFlash();
        // 少加进去的条数必须报出来。以前 success 只看 flash 写没写成功，
        // 于是"只想加 5 条、实际只进去 4 条"会被报成一次干净的保存成功。
        int missing = (int)rulesArray.size() - successCount;
        bool complete = (missing == 0);
        char responseMsg[128];
        if (saved && complete) {
            snprintf(responseMsg, sizeof(responseMsg), "Rules updated, count: %d", successCount);
        } else if (!saved) {
            snprintf(responseMsg, sizeof(responseMsg),
                     "Rules updated, count: %d, flash write failed", successCount);
        } else {
            // 常见原因：这批规则里自己就有重复的 A 信号（同一按键映射到两条），
            // addRule 会拒掉后面的。报出来用户才知道少了哪条。
            snprintf(responseMsg, sizeof(responseMsg),
                     "Rules updated, count: %d, skipped: %d (可能 A 信号在这批里重复)",
                     successCount, missing);
        }
        success = saved && complete;
        return String(responseMsg);
    } else {
        // 单个更新（保持向后兼容）
        uint8_t id = doc["id"] | 0;
        if (id == 0) {
            return "Invalid rule ID or missing 'rules' array";
        }
        
        JsonObject ruleObj = doc["rule"];
        if (ruleObj.isNull()) {
            return "Missing 'rule' field";
        }
        
        IR_MapRule rule;
        if (IRMapper::jsonObjectToRule(ruleObj, rule)) {
            if (mapper.updateRule(id, rule, false)) {
                // 同样把落盘结果反映出来：写失败时不能报成功，否则重启才发现白改
                if (!mapper.saveToFlash()) {
                    return "Rule updated in RAM but flash write failed";
                }
                success = true;
                return "Rule updated";
            } else {
                return "Failed to update rule";
            }
        } else {
            return "Invalid rule format";
        }
    }
}

String APIHandlers::handleGetLogs(Logger& logger) {
    int count = logger.getLogCount();
    size_t jsonSize = MemoryMonitor::calculateLogsJsonSize(count);
    
    if (!MemoryMonitor::checkMemory(jsonSize + 1024)) {
        return "";  // 内存不足
    }
    
    LogEntry entries[MAX_LOG_ENTRIES];
    logger.getLogs(entries, count);
    
    // 构建JSON（只返回data部分）
    size_t totalSize = jsonSize + 256;
    DynamicJsonDocument doc(totalSize);
    JsonArray logArray = doc.to<JsonArray>();
    
    for (int i = 0; i < count; i++) {
        JsonObject entry = logArray.createNestedObject();
        entry["timestamp"] = entries[i].timestamp;
        entry["type"] = entries[i].type;
        entry["content"] = entries[i].content;
    }
    
    String output;
    serializeJson(doc, output);
    return output;
}

String APIHandlers::handleStartLearn(IRLearner& learner, const String& json, bool& success) {
    success = false;
    
    if (!MemoryMonitor::checkMemory(2048)) {
        return "Low Memory";
    }
    
    // 解析JSON
    size_t jsonSize = json.length() + 256;
    if (jsonSize > 1024) jsonSize = 1024;
    if (jsonSize < 256) jsonSize = 256;
    
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, json);
    
    if (error) {
        return "Invalid JSON";
    }
    
    String deviceName = doc["deviceName"] | "";
    String commandNames = doc["commandNames"] | "";
    // 中文「，」「、」「；」与半角「;」都当分隔符，口径与 IRLearner::startDeviceLearn 一致。
    // 在这里归一化是因为**下面这段上限检查**也要按同一个分隔符切，否则「开机，关机」
    // 会被数成 1 个（上限白设），而真正解析时又变 2 个。
    commandNames = IRLearner::normalizeCommandNames(commandNames);
    int timeoutSeconds = doc["timeoutSeconds"] | DEFAULT_LEARN_TIMEOUT_SEC;
    
    if (deviceName.length() == 0) {
        return "Missing 'deviceName' field";
    }
    
    if (commandNames.length() == 0) {
        return "Missing 'commandNames' field";
    }
    
    // 先检查命令数量是否超过限制
    int commandCount = 0;
    if (commandNames.length() > 0) {
        int startPos = 0;
        int commaPos = commandNames.indexOf(',');
        while (commaPos >= 0) {
            String cmdName = commandNames.substring(startPos, commaPos);
            cmdName.trim();
            if (cmdName.length() > 0) {
                commandCount++;
            }
            startPos = commaPos + 1;
            commaPos = commandNames.indexOf(',', startPos);
        }
        // 处理最后一个命令
        if (startPos < commandNames.length()) {
            String cmdName = commandNames.substring(startPos);
            cmdName.trim();
            if (cmdName.length() > 0) {
                commandCount++;
            }
        }
    }
    
    // 检查命令数量限制
    if (commandCount > MAX_LEARN_COMMANDS) {
        char errorMsg[128];
        snprintf(errorMsg, sizeof(errorMsg), "命令数量超过限制，最多支持 %d 个命令，当前有 %d 个", MAX_LEARN_COMMANDS, commandCount);
        return String(errorMsg);
    }
    
    // 启动学习
    if (learner.startDeviceLearn(deviceName, commandNames, timeoutSeconds)) {
        success = true;
        return "学习已启动";
    } else {
        return "Failed to start learning (可能已有学习任务进行中)";
    }
}

String APIHandlers::handleQueryLearnStatus(IRLearner& learner) {
    if (!MemoryMonitor::checkMemory(512)) {
        return "";  // 内存不足
    }
    
    bool isLearning = learner.isLearningDevice();
    
    // 使用简化版本（只返回isLearning, progress, currentCommand）
    DynamicJsonDocument doc(512);
    JsonObject dataObj = doc.to<JsonObject>();
    dataObj["isLearning"] = isLearning;
    
    if (isLearning) {
        String progress = learner.getLearnProgress();
        dataObj["progress"] = progress;
        
        String currentCommand = learner.getCurrentCommandName();
        dataObj["currentCommand"] = currentCommand;
    } else {
        dataObj["progress"] = "";
        dataObj["currentCommand"] = "";
    }
    
    String output;
    serializeJson(doc, output);
    return output;
}

String APIHandlers::handleStopLearn(IRLearner& learner, bool& success) {
    success = false;
    
    if (learner.stopDeviceLearn()) {
        success = true;
        return "Device learn stopped";
    } else {
        return "Device learn is not active";
    }
}

String APIHandlers::handleQueryLearnResult(IRLearner& learner, const String& format,
                                           LearnQueryStatus* outcome) {
    // 三种失败必须能分开，理由见头文件里的注释（不可自愈的假诊断）
    if (outcome) *outcome = LearnQueryStatus::OK;

    int commandCount = learner.getLearnedCommandCount();
    if (commandCount == 0) {
        if (outcome) *outcome = LearnQueryStatus::NO_DATA;
        return "";
    }
    
    // ⚠️⚠️ 门槛必须按**实际需要的最大连续块**定，不能拍脑袋（v1.3.28 修正）。
    //   旧值 4096 ⇒ 实际要求 `maxBlock >= 4608`（checkMemory 是
    //   `freeHeap >= 2048 && maxBlock >= required + 512`）。
    //   而"刚学完"正是内存最低的时刻 —— 学习本身有一笔一次性台阶，实测
    //   `maxBlock` 从 ~11 KB 落到 **2792 B** ⇒ 这个门槛**每次都必然成立不了**
    //   ⇒ 返回 LOW_MEMORY ⇒ 页面 503 "Insufficient memory"
    //   ⇒ 用户报的"学习完成了但不显示学习结果"。
    //   （与 v1.3.25 修 handleGetRecentSignals 是同一类错误：门槛定得比设备
    //    拿得出来的最大连续块还高。）
    //   真正需要的大块只有一个：convertToStringFormat() 内部的 doc
    //   （2 条命令时从 768 B 溢出重试到 1536 B），所以 1024 与它一致；
    //   更深的判断交给它自己的 freeHeap / maxBlock 检查与溢出重试。
    if (!MemoryMonitor::checkMemory(1024)) {
        if (outcome) *outcome = LearnQueryStatus::LOW_MEMORY;
        return "";
    }
    
    String formatLower = format;
    formatLower.toLowerCase();
    const bool wantJson = (formatLower == "json");

    String result;
    // ⭐ v1.3.46：结果已落盘 ⇒ **直接读文件**，不再调 convertToStringFormat()/convertToJSONFormat()。
    //   那两条路各自要造"整份 String + 一块 JsonDocument"，而这正是"刚学完"时最缺的连续内存
    //   （旧的失败形态：LOW_MEMORY ⇒ 503 ⇒ 用户报"学习完成了但不显示结果"）。
    //   现在学习结束时就写进了 LittleFS 并把 RAM 释放了（IRLearner::persistLearnedResult），
    //   这里只剩一次"读文件"的分配。读不到再回退到即时转换（结果还在 RAM 里的老路径）。
    if (learner.isResultPersisted()) {
        result = learner.loadPersistedResult(wantJson);
    } else if (wantJson) {
        result = learner.convertToJSONFormat();
    } else {
        result = learner.convertToStringFormat();
    }
    
    if (result.length() == 0) {
        // 有学习结果、内存也够，却转不出来 ⇒ 这是缺陷，不是"没学过"
        if (outcome) *outcome = LearnQueryStatus::CONVERT_FAILED;
        return "";
    }

    // ⚠️ 这里原来还有一段 `DynamicJsonDocument resultDoc(result.length() + 256)` +
    //   deserializeJson + serializeJson，注释写"解析为JSON对象，然后返回（只返回data部分）"
    //   —— 但输入本来就是 JSON 文本、输出还是同一份 JSON 文本，**是个恒等变换**，
    //   代价却是一块 ~1.4 KB 的连续内存（正好是本设备最缺的那种）。
    //   直接返回 result，对外输出逐字节不变。
    return result;
}

String APIHandlers::handleGetRecentSignals(Logger& logger, unsigned long since) {
    const bool incremental = (since > 0);

    // 增量轮询在"没有新信号"时只回一个 "[]"，所需内存极小，门槛随之降低。
    //
    // ⚠️⚠️ 门槛不能定得比设备**实际能拿到的最大连续块**还高（v1.3.25 修正）：
    //   checkMemory 的实现是 `freeHeap >= 2048 && maxBlock >= required + 512`，
    //   所以 6144 实际要求 **maxBlock ≥ 6656**。
    //   实测开发机 111 进过一次学习模式后落到 freeHeap 5808 / maxBlock 4432
    //   ⇒ 全量加载（前端首屏就是 since=0）**必然**被拒 ⇒ 返回 "" ⇒
    //   processJsonArrayResponse 对空串回 503 "Insufficient memory"
    //   ⇒ 网页上"最近信号"一整块空着，用户看就是"按了没反应"。
    //   （学习模式那次一次性台阶 ≈1.9KB，见 MEMORY.md，是不可回收的。）
    //
    // 新的 1536 / 3072 对应 maxBlock 门槛 2048 / 3584，在 4432 之下留了余量。
    // 后面 doc 的容量是**按实际内容**算的（见下），不是按这个门槛定的，
    // 所以放低门槛不会让 doc 挤爆堆 —— 门槛只管"够不够开工"。
    // ⭐⭐ v1.3.49：门槛按**这一路实际要处理的量**给，不再是一个写死的常数。
    //
    //   旧写法 `checkMemory(incremental ? 1536 : 3072)` 要求 `maxBlock ≥ 2048 / 3584`
    //   —— 连**列表为空**（响应只有 65 字节的 `[]`）也照样被拒。212（带 MQTT）空闲
    //   `maxBlock` 实测 **3120 < 3584** ⇒ 打开面板「最近信号」**每一次**都回
    //   503 "Insufficient memory" ⇒ 用户看到「设备内存不足，请稍后再试」。
    //   （另：`checkMemory(n)` 的实际判据是 `maxBlock ≥ n + 512`，见 memory_monitor.cpp。）
    //
    //   现在改成 `min(旧常数, 512 + 时序码 Base64 实际总量)`：
    //     · 空列表 / 没有时序码 ⇒ 只要"能开工"的水位（512 ⇒ maxBlock ≥ 1024）；
    //     · 有内容 ⇒ 按 Base64 实际长度给（Logger 那个值已含 2× 多副本安全系数，
    //       对应深拷贝 + ArduinoJson 副本 + 响应文本三份）；
    //     · ⛔ **只降不升** —— 上限仍是压测出来的旧常数，绝不会因此要得更多。
    //   内容量算不准时的后果是不对称的：多要（宁可 503，用户重试）没问题，
    //   少要才会在发射/序列化中途失败（那是"发出去一半"的错数据）。
    const size_t legacyGate = incremental ? 1536 : 3072;
    const size_t contentGate = 512 + (size_t)logger.getRecentTimingBytes();
    const size_t gate = (contentGate < legacyGate) ? contentGate : legacyGate;
    if (!MemoryMonitor::checkMemory(gate)) {
        return "";  // 内存不足
    }
    
    SignalSequence sequences[MAX_RECENT_SIGNALS];
    int count = logger.getRecentSignalSequences(sequences, MAX_RECENT_SIGNALS);
    
    // ⚠️ 容量必须按**实际内容**算，不能按"条数 × 500"猜（v1.3.24）
    // ArduinoJson 装不下时是**静默丢字段**、不报错。原来的估法在两条路上下都错：
    //   ① 估的是"序列化后的文本长度"，但 DynamicJsonDocument 要的是**内部表示**的字节数
    //      （每个键和值都占 slot，嵌套对象/数组还各有一份开销），实际约为文本的 1.5~2 倍
    //      ⇒ 估 3072 装不下 2.5 KB 的时序码 JSON，最靠后的条目被拦腰截断；
    //   ② 原来的 `256 + n*500` 与内容量完全无关。
    // 症状：sequenceToJSON 原先把 timestamp / matched 写在最后 ⇒ 最先被丢
    // ⇒ 前端 formatTime(undefined) 返回空串，表现为"有的信号显示时间，有的不显示，
    // 刷新一次变一次"。现在字段顺序已改（unified_signal.cpp），但容量仍要算准。

    // 单条序列的**内部表示**字节数（宁可多算：多算只是 doc 大一点，
    // 少算会静默丢字段 —— 那才是 bug 根因）：
    //   每信号 ≈ 220 B slot 开销（6 个键值对 + 嵌套对象本身）
    //   + useTiming 时加 timingBase64 的长度 + 字符串本身（约 1.3 倍因 slot 也要存）
    //   每序列外壳 ≈ 160 B（嵌套对象 + 2 个数组 + timestamp + matched）
    //
    // ⚠️ 预算不能超过堆的半数：doc 之后 processJsonArrayResponse 还要把结果
    // 拼进响应 String（api_response.cpp 的 processJsonArrayResponse），
    // 那份是 1 份 JSON 文本；doc 是 1.5~2 份 ⇒ 加起来要 3 倍。
    //
    // ⚠️⚠️ 而且 getRecentSignalSequences 里的 SignalSequence[5] 已经把每条时序码
    // **深拷贝了一份**（copySequence → copySignal → String 赋值），
    // 所以请求期间时序码在堆上是**双份**：Logger 缓冲一份 + API 层一份。
    //
    // ⚠️⚠️ 阈值必须按**配了 MQTT 的正式机**定（v1.3.24 二次修正）：
    //   实测空闲堆 开发机 111 = 11712 B，正式机 212（有 MQTT）= **10592 B**。
    //   预算 2048 时本函数最坏 = 时序码双份 4096 + doc 2048 + 响应 ~2100
    //   ≈ 8.2 KB，212 上留 2.4 KB 余量给 MQTT 与其它。
    // 增量只多装"自 since 以来的新条目"，所以预算可以比全量小；但**不能小到装不下
    // 一条满帧时序码**：那个的下界是 128 + 160 + 220 + 576 = 1084。
    size_t budget = (incremental ? 2048 : 3072);
    size_t used = 128;   // 外层 [] 与 ArduinoJson 自身开销
    int firstFit = count;   // 从最新一条往回装，firstFit 是"装得下的最旧一条下标"

    for (int i = count - 1; i >= 0; i--) {
        if (incremental && sequences[i].timestamp <= since) continue;
        size_t need = 160;
        for (int j = 0; j < sequences[i].signalCount; j++) {
            need += 220;
            if (sequences[i].signals[j].useTiming) {
                // ⚠️⚠️ 这里**绝对不能 ×2**（v1.3.24 评审揪出的 P0）。
                //   这里算的是 **JsonDocument 的容量**，而 doc 里存的是单字节字符 ——
                //   ESP8266 的 String 是 char 数组，不是 UCS-2（core 的 WString.h，
                //   2026-10-03 查源码确认）。
                //   按 ×2 算会高估一倍：风扇实测 144/192/216 点 → Base64 384/512/576 字符
                //   ⇒ need 比真值大 384~576 ⇒ `used + need > budget` 在**最新一条**上就成立
                //   ⇒ 立刻 break ⇒ firstFit 停在 count ⇒ 函数返回 "[]"。
                //   现象：切页签 / 1.2 s 实时监视轮询（都走增量路径）**永远看不到时序码**，
                //   只有整页刷新看得到 —— 正是用户报的"按风扇遥控器按钮，最近信号里没有"。
                need += sequences[i].signals[j].timingBase64.length();
            }
        }
        // ⭐ **最新一条无条件放行**：哪怕它一条就超预算也必须发出去。
        //   否则"刚按了遥控器、列表里却什么都没有"就是必然结果 —— 那正是本次要修的故障。
        //   判据：firstFit 仍 == count ⇒ 说明还没装进任何一条 ⇒ 当前这条就是最新的那条。
        if (used + need > budget && firstFit != count) break;
        used += need;
        firstFit = i;
    }
    
    if (firstFit >= count) {
        return "[]";   // 没有任何一条符合条件（正常情况：增量轮询无新信号）
    }
    
    // ⚠️ 真的装不下时**丢最旧的**，而不是让 JSON 被截断。
    // 截断产出的是"半条记录"——前端会拿它当完整信号用（设为 A/B、发送），
    // 那是错数据；丢最旧的只是少看几条老的，语义上安全。
    // "最近信号"本来就是环形缓冲（MAX_RECENT_SIGNALS=5），
    // 装不下时最旧的本来就是最该被淘汰的。
    // ⚠️ 必须限流：这条挂在 1.2 s 一次的增量轮询路径上，真的装不下时会一直打 ——
    //   而 ESP8266 的 Serial 是同步阻塞的，刷屏本身就是 WDT 复位的成因之一（本轮已踩到）。
    if (firstFit > 0) {
        static unsigned long lastDropWarnMs = 0;
        unsigned long nowMs = millis();
        if (nowMs - lastDropWarnMs >= SERIAL_RATE_LIMIT_MS) {
            lastDropWarnMs = nowMs;
            Serial.printf_P(PSTR("[API] 最近信号 JSON 装不下，丢弃最旧的 %d 条（保留 %d 条）\n"),
                          firstFit, count - firstFit);
        }
    }
    
    DynamicJsonDocument doc(used);
    JsonArray sequencesArray = doc.to<JsonArray>();
    
    for (int i = firstFit; i < count; i++) {
        if (incremental && sequences[i].timestamp <= since) continue;
        JsonObject seqObj = sequencesArray.createNestedObject();
        SignalUtils::sequenceToJSON(sequences[i], seqObj, true);
    }
    
    String output;
    serializeJson(doc, output);
    return output;
}

// 把各入口收到的"信号 payload"归一成 forwarder.parseAndSendSignal() 认的形式。
// 为什么必须只有一份：HTTP / MQTT / SmartIR 是三条独立入口，以前 MQTT 自己写了
// 一套 3 分支的 JSON 解包，HTTP 侧只有 trim + parse ⇒ 同一个 JSON 在网页上发得出去、
// 在 MQTT 上发不出去，排查时完全看不出是哪一边的问题。
String APIHandlers::normalizeSignalPayload(const String& payload) {
    String trimmed = payload;
    trimmed.trim();
    if (trimmed.length() == 0) return String();
    if (trimmed.charAt(0) != '{') {
        return trimmed;   // Base64 / 时序数组 / 协议格式，原样交给 forwarder
    }

    size_t jsonSize = trimmed.length() + 256;
    if (jsonSize > 1024) jsonSize = 1024;
    if (!MemoryMonitor::checkMemory(jsonSize + 512)) {
        return String();   // 内存不足 ⇒ 调用方会报 Low Memory
    }

    DynamicJsonDocument doc(jsonSize);
    DeserializationError err = deserializeJson(doc, trimmed);
    if (err) {
        return trimmed;    // 解不动就原样交下去，由 forwarder 报"格式不认识"
    }

    if (doc.containsKey("signals")) {
        return trimmed;    // 已经是序列格式
    }

    // 单条信号：可能裸着（{"signal":{...}}），也可能包在完整的 send_signal 命令里
    // （{"action":"send_signal","signal":{...}}）—— 两种情况归一成同一个序列格式。
    // 注：原来 MQTT 侧为此写了两段一模一样的代码，而第二段（按 action 判断）永远
    // 走不到，因为第一段的条件更宽。
    JsonObject signalObj = doc["signal"];
    if (!signalObj.isNull()) {
        DynamicJsonDocument signalDoc(512);
        JsonArray signalsArray = signalDoc.createNestedArray("signals");
        signalsArray.add(signalObj);
        String out;
        serializeJson(signalDoc, out);
        return out;
    }

    return trimmed;
}

String APIHandlers::handleSendSignal(IRForwarder& forwarder, const String& payload, bool& success) {
    success = false;

    // ── 埋点（v1.3.30，只加日志）：请求**已经进到业务层**的证据。
    //   配合 IRForwarder::sendSequence() 的 [TX] start/end 三段定位：
    //     ① 连 [API send] 都没有        ⇒ 卡在 HTTP/TCP 层（请求没送到）
    //     ② 有 [API send] 没 [TX] start ⇒ 卡在 JSON 解析 / 内存检查
    //     ③ 有 [TX] start 没 [TX] end   ⇒ 卡在发射内部（时间/中断问题，非内存）
    Serial.printf_P(PSTR("[API send] len=%u heap=%u blk=%u\n"),
                  (unsigned)payload.length(),
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxFreeBlockSize());

    if (!MemoryMonitor::checkMemory(2048)) {
        return "Low Memory";
    }

    // 总堆下限（v1.3.39）。checkMemory() 只看 maxBlock / 使用率，实测总堆只剩 3.6 KB 时
    // **照样放行**，而那次放行的结果是**软看门狗硬复位**（resetReason=3）。
    // ⛔ 判据只能用 freeHeap（按 maxBlock 看不单调：blk=2328 能跑通、blk=2744 崩）；
    // ⛔⛔ 且只能卡在这个**请求入口**，别挪到 IRForwarder::sendSequence()
    //    —— 发射时堆比这里低约 1.4 KB，挪过去会误杀正常发送。完整依据见 memory_monitor.h。
    if (!MemoryMonitor::hasSendHeadroom("API send")) {
        return "Low Memory";
    }
    
    String normalized = normalizeSignalPayload(payload);
    if (normalized.length() == 0) {
        return "Empty payload";
    }
    
    // 统一使用parseAndSendSignal，支持JSON和字符串两种格式
    if (forwarder.parseAndSendSignal(normalized)) {
        success = true;
        return "Signal sent";
    } else {
        return "Failed to send signal";
    }
}

// 转发记录：回答"刚才那条信号到底转发没有、转到哪去了"
String APIHandlers::handleGetForwardRecords(IRForwarder& forwarder) {
    if (!MemoryMonitor::checkMemory(1024)) {
        return "";  // 内存不足
    }

    ForwardRecord records[MAX_FORWARD_RECORDS];
    int count = forwarder.getForwardRecords(records, MAX_FORWARD_RECORDS);

    // 每条约 130 字节（aCode/aProtocol/ruleId/bCount/durationMs/latencyMs/result 七个字段 + 括号）
    size_t jsonSize = 256 + count * 140;
    if (jsonSize > 1400) jsonSize = 1400;
    if (jsonSize < 384) jsonSize = 384;

    DynamicJsonDocument doc(jsonSize);
    JsonArray arr = doc.to<JsonArray>();

    for (int i = 0; i < count; i++) {
        JsonObject o = arr.createNestedObject();
        o["ms"]         = records[i].timestamp;     // 设备开机毫秒（前端算"多久以前"）
        o["ageSec"]     = (unsigned long)((millis() - records[i].timestamp) / 1000);  // 距今多少秒（设备端算更准）
        o["aCode"]      = records[i].aCode;         // A 侧码值（时序码时为 0）
        o["aProtocol"]  = records[i].aProtocol;     // 255 = 时序码
        o["ruleId"]     = records[i].ruleId;        // -1 = 未命中任何规则
        o["bCount"]     = records[i].bCount;
        o["durationMs"] = records[i].durationMs;
        // 解码完成 → 开始准备发射的毫秒数。用来区分"慢在等"与"慢在发"：
        // durationMs 是发（波形本身），latencyMs 是等（AB 隔离 20ms + 5ms 轮询 + 前一次发射没结束）。
        o["latencyMs"]  = records[i].latencyMs;
        o["result"]     = records[i].result;        // 0=已发送 1=未匹配 2=失败 3=转发关闭
    }

    String output;
    serializeJson(doc, output);
    return output;
}

// 试跑：立刻匹配并发射，把"为什么没转发"直接说清楚
String APIHandlers::handleForwardTest(IRForwarder& forwarder, const String& payload, bool& success) {
    success = false;

    if (!MemoryMonitor::checkMemory(1024)) {
        return "Low Memory";
    }

    // 试跑也是**真发射**（testForward / forwardSignal → sendSequence），所以同样要有总堆下限：
    // 少了它，堆紧的时候点「试发」会和以前点「发送」一样直接软看门狗硬复位。
    // 判据与用法见 memory_monitor.h 的 hasSendHeadroom()。
    if (!MemoryMonitor::hasSendHeadroom("API forward/test")) {
        return "Low Memory";
    }

    String trimmed = payload;
    trimmed.trim();
    if (trimmed.length() == 0) {
        return "缺少信号";
    }

    // 兼容两种入参：{"a":"1,0x00003039"} 或纯字符串 "1,0x00003039"
    // 另外支持 {"a":"...","via":"recv"} —— 走真实入站路径（量转发延迟用）
    String sig;
    String via;
    if (trimmed.startsWith("{")) {
        StaticJsonDocument<256> doc;
        if (deserializeJson(doc, trimmed)) {
            return "JSON 格式错误";
        }
        sig = String((const char*)(doc["a"] | ""));
        via = String((const char*)(doc["via"] | ""));
    } else {
        sig = trimmed;
    }
    sig.trim();
    if (sig.length() == 0) {
        return "缺少信号（a）";
    }

    const bool viaRecv = (via == "recv");
    // 用"记录序号有没有往前走"判断本次动作到底产生没产生新记录。
    // 以前是拿"最新一条记录"去猜，只在它的时间戳超过 3 秒时才判丢弃 —— 实测
    // （120ms 节奏注入 8 次）被忙碌闸门挡掉的那几次全部误报成"已匹配规则并发出"。
    const uint16_t serialBefore = forwarder.forwardRecordSerial();
    bool ok = viaRecv ? forwarder.forwardSignalViaPayload(sig)
                      : forwarder.testForwardPayload(sig);
    const bool produced = (forwarder.forwardRecordSerial() != serialBefore);

    if (!produced) {
        // 没产生新记录 = 信号根本没走到"匹配规则"这一步。
        // via=recv 时是忙碌闸门（发射中 / 还在发射后静默期内）或自收回声判据挡的；
        // 试跑路径则是字符串压根没解析成信号。
        return viaRecv ? "被挡下：上一次发射还没结束 / 还在发射后静默期内"
                       : "信号无法解析（格式应为 \"协议,码值\"，如 \"1,0x11B08F7\"）";
    }

    // 结论从"最新一条转发记录"里取，这样"未匹配"和"发射失败"能分开说。
    // 本次一定写过记录，而它是刚写的 ⇒ 最新一条就是它。
    String msg = ok ? "已匹配规则并发出" : "未匹配到规则";
    ForwardRecord recs[1];
    if (forwarder.getForwardRecords(recs, 1) == 1) {
        switch (recs[0].result) {
            case FORWARD_RESULT_SENT:    msg = "已匹配规则并发出"; break;
            case FORWARD_RESULT_NOMATCH: msg = "没有任何规则匹配这个信号"; break;
            case FORWARD_RESULT_FAILED:  msg = "匹配到了规则，但发射失败"; break;
            case FORWARD_RESULT_DISABLED:msg = "转发总开关已关闭"; break;
            default: break;
        }
        success = (recs[0].result == FORWARD_RESULT_SENT);
    } else {
        success = ok;
    }
    return msg;
}
