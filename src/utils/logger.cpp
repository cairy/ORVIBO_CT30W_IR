#include "logger.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <utility>      // std::move（JSON 扩容时转移内存池的所有权，而不是深拷贝）

Logger::Logger() : log_index(0), log_count(0), needsSave(false),
                   recentSequenceIndex(0), recentSequenceCount(0),
                   isBuildingSequence(false), lastSignalTime(0), lastSignalMatched(false),
                   logPublishCallback(nullptr) {
    for (int i = 0; i < MAX_LOG_ENTRIES; i++) {
        logs[i].valid = false;
        logs[i].type[0] = '\0';
        logs[i].content[0] = '\0';
    }
    for (int i = 0; i < MAX_RECENT_SIGNALS; i++) {
        recentSequences[i].clear();
    }
    currentSequence.clear();
}

bool Logger::init() {
    if (!LittleFS.begin()) {
        return false;
    }
    loadFromFlash();
    return true;
}

void Logger::log(const String& type, const String& content) {
    // 优化：内存压力大时跳过非关键日志
    // ⚠️ 门槛从 4096 降到 1024（v1.3.17）：配了 MQTT 之后空闲堆常驻只有 3~5 KB，
    // 4096 会把**所有非 error 日志全部丢掉** —— 表现是 HA 的"日志"传感器只剩 error，
    // 面板的最近日志也一直是旧记录。Logger 本身只用定长 char 数组、不额外分配堆，
    // 真正需要保护的是后面的 Flash 保存（那条另有门槛），所以这里不用卡那么高。
    if (ESP.getFreeHeap() < 1024 && type != "error") {
        return;
    }
    
    unsigned long now = millis();
    
    // 循环覆盖
    logs[log_index].timestamp = now;
    // 使用strncpy安全复制，确保以null结尾
    const char* typeStr = type.c_str();
    const char* contentStr = content.c_str();
    if (typeStr != nullptr) {
        strncpy(logs[log_index].type, typeStr, MAX_LOG_TYPE_LEN - 1);
        logs[log_index].type[MAX_LOG_TYPE_LEN - 1] = '\0';
    } else {
        logs[log_index].type[0] = '\0';
    }
    if (contentStr != nullptr) {
        strncpy(logs[log_index].content, contentStr, MAX_LOG_CONTENT_LEN - 1);
        logs[log_index].content[MAX_LOG_CONTENT_LEN - 1] = '\0';
    } else {
        logs[log_index].content[0] = '\0';
    }
    logs[log_index].valid = true;
    
    log_index = (log_index + 1) % MAX_LOG_ENTRIES;
    if (log_count < MAX_LOG_ENTRIES) {
        log_count++;
    }
    
    // 串口输出（统一格式：与MQTT日志传感器一致）
    Serial.printf_P(PSTR("[%s] %s\n"), logs[log_index == 0 ? MAX_LOG_ENTRIES - 1 : log_index - 1].type, 
                  logs[log_index == 0 ? MAX_LOG_ENTRIES - 1 : log_index - 1].content);
    
    // 事件驱动：推送最新日志到MQTT（如果回调已设置）
    if (logPublishCallback != nullptr) {
        // 获取最新日志条目索引
        int latestIndex = (log_index == 0) ? (MAX_LOG_ENTRIES - 1) : (log_index - 1);
        logPublishCallback(logs[latestIndex].type, logs[latestIndex].content);
    }
    
    // 优化：只在达到阈值或内存压力大时才标记保存
    // 如果日志数量达到一定阈值（MAX_LOG_ENTRIES的2/3）或内存使用率较高，才标记保存
    // 这样可以减少Flash写入次数，延长Flash寿命
    static int logSaveThreshold = (MAX_LOG_ENTRIES * 2) / 3;  // 日志数量阈值（MAX_LOG_ENTRIES 的 2/3 = 6 条）
    if (log_count >= logSaveThreshold) {
        needsSave = true;
    }
    // 注意：错误日志总是立即标记保存（重要信息）
    if (type == "error") {
        needsSave = true;
    }
}

// 优化：添加const char*版本，避免String对象创建
void Logger::log(const char* type, const char* content) {
    // 优化：内存压力大时跳过非关键日志（门槛说明见上面 String 版）
    if (ESP.getFreeHeap() < 1024 && (type == nullptr || strcmp(type, "error") != 0)) {
        return;
    }
    
    unsigned long now = millis();
    
    // 循环覆盖
    logs[log_index].timestamp = now;
    // 使用strncpy安全复制，确保以null结尾
    if (type != nullptr) {
        strncpy(logs[log_index].type, type, MAX_LOG_TYPE_LEN - 1);
        logs[log_index].type[MAX_LOG_TYPE_LEN - 1] = '\0';
    } else {
        logs[log_index].type[0] = '\0';
    }
    if (content != nullptr) {
        strncpy(logs[log_index].content, content, MAX_LOG_CONTENT_LEN - 1);
        logs[log_index].content[MAX_LOG_CONTENT_LEN - 1] = '\0';
    } else {
        logs[log_index].content[0] = '\0';
    }
    logs[log_index].valid = true;
    
    log_index = (log_index + 1) % MAX_LOG_ENTRIES;
    if (log_count < MAX_LOG_ENTRIES) {
        log_count++;
    }
    
    // 串口输出（统一格式：与MQTT日志传感器一致）
    Serial.printf_P(PSTR("[%s] %s\n"), logs[log_index == 0 ? MAX_LOG_ENTRIES - 1 : log_index - 1].type, 
                  logs[log_index == 0 ? MAX_LOG_ENTRIES - 1 : log_index - 1].content);
    
    // 事件驱动：推送最新日志到MQTT（如果回调已设置）
    if (logPublishCallback != nullptr) {
        // 获取最新日志条目索引
        int latestIndex = (log_index == 0) ? (MAX_LOG_ENTRIES - 1) : (log_index - 1);
        logPublishCallback(logs[latestIndex].type, logs[latestIndex].content);
    }
    
    // 优化：只在达到阈值或内存压力大时才标记保存
    static int logSaveThreshold = (MAX_LOG_ENTRIES * 2) / 3;  // 日志数量阈值（MAX_LOG_ENTRIES 的 2/3 = 6 条）
    if (log_count >= logSaveThreshold) {
        needsSave = true;
    }
    // 注意：错误日志总是立即标记保存（重要信息）
    if (type != nullptr && strcmp(type, "error") == 0) {
        needsSave = true;
    }
}

void Logger::logReceive(const String& signal_info) {
    log("receive", signal_info);
}

void Logger::logTransmit(const String& signal_info) {
    log("transmit", signal_info);
}

void Logger::logError(const String& error_msg) {
    log("error", error_msg);
}

void Logger::logLearn(const String& signal_info) {
    log("learn", signal_info);
}

void Logger::logReceiveSignal(const UnifiedSignal& signal, bool matched) {
    unsigned long now = millis();

    // ⚠️ 重复帧去重（v1.3.24）
    //
    // 背景：不少遥控器（实测艾美特风扇）一次按键会**连发 3~5 遍**同一帧，
    // 遍与遍间隔 15~18ms。时序码兜底启用后每遍都是整段 Base64 堆分配
    // （实测 191 点 ≈ 512 字符）⇒ 不去重的话一次按键就能灌进 2.5 KB。
    //
    // ⚠️⚠️ 去重时**绝不能动 lastSignalTime**。它表示"上一条**有效**信号的时刻"，
    // 后面所有 interval 计算（序列聚合、5 秒切序列）都依赖它。
    // 第一次写成"去重时也刷新 lastSignalTime"，后果是：
    //   · 下一条真实信号的 interval 变成"距最后一次连发"而不是"距上次有效信号"
    //   · 连发 5 遍期间 isBuildingSequence 恒为 true，currentSequence 永远 1 条
    //   · 若下一条是**不同**信号且间隔落在 1~5 秒之间，会被误并进上一条序列
    // 去重就是纯粹"什么都不做"——连时间戳都不碰，语义最干净。
    if (signal.useTiming && lastSignalTime > 0 && isBuildingSequence &&
        currentSequence.signalCount > 0) {
        unsigned long gap = now - lastSignalTime;
        if (gap <= TIMING_DEDUP_WINDOW_MS &&
            SignalUtils::signalsEqual(currentSequence.signals[currentSequence.signalCount - 1],
                                      signal)) {
            return;   // 同一遍帧的重复发射：静默丢弃
        }
    }

    // 计算与前一个信号的时间间隔
    unsigned long interval = 0;
    if (lastSignalTime > 0) {
        // 无符号减法本身就正确处理回绕（millis() 约 49.7 天回绕一次）。
        // 原来那个分支不但多余，算出来还比正确值**小 1 毫秒**。
        interval = now - lastSignalTime;
        
        // 如果间隔超过阈值（5秒），完成当前序列并开始新序列
        if (interval > SIGNAL_INTERVAL_RESET_THRESHOLD_MS) {
            if (isBuildingSequence) {
                finalizeCurrentSequence();
            }
            interval = 0;
            lastSignalTime = 0;
        }
    }
    
    // 序列检测逻辑
    if (!isBuildingSequence) {
        // 开始新序列（第一个信号，不使用第一个信号前间隔）
        currentSequence.clear();
        currentSequence.timestamp = now;
        currentSequence.matched = matched;
        currentSequence.addSignal(signal, 0, false);
        isBuildingSequence = true;
        lastSignalTime = now;
        lastSignalMatched = matched;
    } else {
        // 判断是否加入当前序列
        if (interval < SIGNAL_INTERVAL_MAX_MS && canJoinSequence(signal)) {
            // 加入当前序列（标准模式：interval 是前一个信号和当前信号之间的间隔）
            currentSequence.addSignal(signal, interval, false);
            lastSignalTime = now;
            lastSignalMatched = matched;  // 更新匹配状态（使用最后一个信号的匹配状态）
        } else {
            // 完成当前序列，开始新序列
            finalizeCurrentSequence();
            currentSequence.clear();
            currentSequence.timestamp = now;
            currentSequence.matched = matched;
            currentSequence.addSignal(signal, 0, false);
            isBuildingSequence = true;
            lastSignalTime = now;
            lastSignalMatched = matched;
            interval = 0;
        }
    }
    
    // ⚠️ 串口输出限流（v1.3.24，修"一按遥控器就重启"）
    //
    // 崩溃真因不是堆，是**串口阻塞**。Serial.printf 在 ESP8266 上是同步的：
    // UART FIFO 满之后 busy-wait 等发送完成。一条 [receive] 约 130 字符，
    // 115200 波特下 ≈ **11 ms**；再加上 [timing] 那条 ≈ 6 ms，
    // 合计 17 ms —— 而红外接收任务的间隔只有 10 ms（IR_RECEIVER_INTERVAL）。
    // ⇒ loop() 被串口拖住，永远喂不到 WDT ⇒ 复位。
    // 实测 2026-10-02：一次按键打出 11214 行串口，设备直接重启
    // （resetReason=4 software-restart，而堆其实健康、freeHeap 还有 10 KB）。
    //
    // 教训：**热路径上的串口输出必须限流**。调试信息打得越多、越快，
    // 越容易把主循环拖垮 —— 这与堆管理无关，是另一类死法。
    // 这里改成每秒最多 SERIAL_RATE_LIMIT_MS 打一条，且把行首压到 60 字符以内。
    static unsigned long lastPrintMs = 0;
    const bool rateLimited = (now - lastPrintMs) < SERIAL_RATE_LIMIT_MS;
    if (rateLimited) return;

    // 直接串口输出（轻量操作，不写入日志缓冲区）
    if (!rateLimited && signal.useTiming) {
        // 只打长度不打 Base64 前缀：16 字符前缀对排障没用，纯粹是 17ms 里的大头
        const char* base64Str = signal.timingBase64.c_str();
        (void)base64Str;   // 保留引用，避免"未使用"警告掩盖真实语义
        int base64Len = signal.timingBase64.length();

        if (interval > 0) {
            Serial.printf_P(PSTR("[recv] T:%d pts B64:%d M:%s I:%lums\n"),
                         signal.timingCount, base64Len,
                         matched ? "Y" : "N", interval);
        } else {
            Serial.printf_P(PSTR("[recv] T:%d pts B64:%d M:%s\n"),
                         signal.timingCount, base64Len, matched ? "Y" : "N");
        }
        lastPrintMs = now;
    } else if (!rateLimited) {
        if (interval > 0) {
            Serial.printf_P(PSTR("[recv] P:%u D:%08X M:%s I:%lums\n"),
                         signal.protocol, signal.data,
                         matched ? "Y" : "N", interval);
        } else {
            Serial.printf_P(PSTR("[recv] P:%u D:%08X M:%s\n"),
                         signal.protocol, signal.data, matched ? "Y" : "N");
        }
        lastPrintMs = now;
    }
}

int Logger::getRecentSignalSequences(SignalSequence* sequences, int max_count) {
    if (sequences == nullptr || max_count <= 0) {
        return 0;
    }
    
    // 如果正在构建序列，先完成它
    if (isBuildingSequence) {
        finalizeCurrentSequence();
    }
    
    int copy_count = (max_count < recentSequenceCount) ? max_count : recentSequenceCount;
    if (copy_count <= 0) return 0;

    // ⚠️⚠️ 必须按 timestamp 升序输出，不能按环形下标绕圈（v1.3.24 修）
    //
    // 引入 dropOldestSequence（堆预算闸）后，缓冲里会出现**空洞** ——
    // 被淘汰的可能是中间某条，而不是最旧的。下标绕圈读会把空洞后面的
    // 条目"提前"，于是读出来的顺序与实际写入顺序不一致。
    // 推演实测：2500 帧时序码压测后，读出的 timestamp 明显乱序。
    //
    // 为什么乱序是**功能性 bug**而不只是显示问题：前端 loadRecent() 用
    // `since = 本地最大 timestamp` 做增量拉取（web/app.js）。乱序会让
    // "最新"那条排在中间 ⇒ 下一轮 since 偏小 ⇒ 反复拉重复条目；
    // 极端情况下新信号的 timestamp 比 since 小 ⇒ **永远拉不到新信号**，
    // 页面看着像"实时监视坏了"。
    //
    // 环形缓冲只有 5 个槽，插入排序足够，无需 std::sort。
    int order[MAX_RECENT_SIGNALS];
    int n = 0;
    for (int i = 0; i < MAX_RECENT_SIGNALS; i++) {
        if (recentSequences[i].valid) order[n++] = i;
    }
    for (int i = 1; i < n; i++) {          // 按 timestamp 升序插入排序
        int key = order[i];
        int j = i - 1;
        while (j >= 0 && recentSequences[order[j]].timestamp > recentSequences[key].timestamp) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    int written = 0;
    for (; written < copy_count && written < n; written++) {
        SignalUtils::copySequence(recentSequences[order[written]], sequences[written]);
    }

    return written;
}

void Logger::finalizeCurrentSequence() {
    if (!isBuildingSequence || currentSequence.signalCount == 0) {
        return;
    }

    // 更新匹配状态（使用最后一个信号的匹配状态）
    currentSequence.matched = lastSignalMatched;
    currentSequence.valid = true;

    // ⚠️ 时序码堆预算闸（v1.3.24）—— 防"一按遥控器就抽干堆"的最后一道闸。
    // 背景：时序码兜底启用后，recentSequales 里的每条 Base64 都是整段堆分配
    // （191 点 ≈ 512 字符），5 条 × 6 信号 = 最多 30 条 ⇒ 最坏 15 KB，
    // 而空闲堆只有 11.6 KB。去重已经挡掉了"同一次按键的连发"，
    // 但用户连按不同按键仍会累积 ⇒ 这里按**总字符数**封顶，
    // 超了就丢最旧的（环形缓冲里最旧的本来就是最该淘汰的）。
    // 判据用字符数而不是条数：协议信号不占堆（data 是定长 uint32），
    // 混在同一个缓冲里，用条数算会误伤。
    if (!fitsTimingBudget(currentSequence)) {
        // ⚠️ 同样限流：这条也在热路径上（每次 finalize 都可能打），
        // 而 finalize 由"每收到一条信号"触发，频率等于遥控器连发频率。
        static unsigned long lastBudgetWarnMs = 0;
        unsigned long nowMs = millis();
        if (nowMs - lastBudgetWarnMs >= SERIAL_RATE_LIMIT_MS) {
            lastBudgetWarnMs = nowMs;
            Serial.println(F("[最近信号] 时序码堆预算已满，丢弃最旧一条"));
        }
        dropOldestSequence();
    }

    // 记录到最近序列缓冲区（环形缓冲区）
    SignalUtils::copySequence(currentSequence, recentSequences[recentSequenceIndex]);

    recentSequenceIndex = (recentSequenceIndex + 1) % MAX_RECENT_SIGNALS;
    if (recentSequenceCount < MAX_RECENT_SIGNALS) {
        recentSequenceCount++;
    }

    // 刚放进去这条就把预算撑爆了 ⇒ 立刻丢最旧的（可能就是它自己，
    // 当缓冲里原本就已满时）。不丢的话下一次分配就会 OOM。
    while (recentSequenceCount > 1 && recentTimingBytes() > RECENT_TIMING_BUDGET_BYTES) {
        dropOldestSequence();
    }

    // 重置构建状态
    isBuildingSequence = false;
    currentSequence.clear();
}

// ⭐ 时序码只能**独占**一条序列（v1.3.24）
//
// 为什么：多信号序列是"A 信号 → 发多个 B 信号"那个机制的载体，
// 而时序码从来没有当过 A 信号的角色（它来自"库认不出"的兜底路径），
// 一次按键 = 一条时序码，把它和后续信号拼成序列没有意义。
//
// 更要紧的是**堆**：实测 191 点 → Base64 512 字符。ESP8266 的 String 是
// **char 数组**（1 字节/字符 —— core 的 WString.h，_sso/_ptr 都是 char；
// ⛔ 旧注释写"UCS-2、每字符 2 字节"是错的，2026-10-03 查源码更正）
// ⇒ 单条 Base64 约 512 B；但同一份数据在请求期间还会被深拷贝一份到 API 层、
// 再被 ArduinoJson 复制进 doc ⇒ 闸门按 **2×** 计（≈1 KB/条）。
// 若允许时序码序列装 6 条，环形缓冲最坏 5 × 6 × 1 KB = **30 KB**，
// 而空闲堆只有 11.7 KB ⇒ 必然 OOM。
// 限制成"时序码独占 1 条"后最坏 5 × 1 KB = 5 KB，留足余量。
bool Logger::canJoinSequence(const UnifiedSignal& signal) const {
    if (currentSequence.signalCount >= MAX_B_SIGNALS_PER_RULE) return false;
    // 序列里已有时序码 ⇒ 不让任何信号再加进来（那一条已经占了 1 KB）
    // ⚠️ 必须带 signalCount > 0 守卫：signalCount==0 时 signals[0] 是上一条序列
    //    clear() 后残留的值，读它会得到一个与当前序列无关的判据。
    if (currentSequence.signalCount > 0 && currentSequence.signals[0].useTiming) return false;
    // 新信号是时序码而序列里已有别的 ⇒ 另起一条
    if (signal.useTiming && currentSequence.signalCount > 0) return false;
    return true;
}

int Logger::recentTimingBytes() const {
    // ⚠️ 系数 2 是**刻意的安全余量**，不是"String 是 UCS-2"那个错误前提
    //   （2026-10-03 查 core 源码更正：ESP8266 的 String 是 char 数组，1 字节/字符，
    //    WString.h 的 _sso/_ptr 都是 char）。
    //   同一份 Base64 在请求期间会在堆上存在**多份**：① 本环形缓冲这份；
    //   ② getRecentSignalSequences 深拷贝到 API 层那份；③ 进 doc 时 ArduinoJson
    //   还会再复制一份字符串。所以按 2× 计是保守但不虚的。
    //   191 点 → 512 字符 → 按 1 KB/条计。
    //   ⛔ 但注意：**不要**拿同一个系数去算 JsonDocument 的容量 —— 那里必须按
    //   1× 算（doc 存的是单字节字符），否则增量查询会一条都装不下（见 api_handlers.cpp）。
    int total = 0;
    for (int i = 0; i < MAX_RECENT_SIGNALS; i++) {
        if (!recentSequences[i].valid) continue;
        for (int j = 0; j < recentSequences[i].signalCount; j++) {
            if (recentSequences[i].signals[j].useTiming) {
                total += recentSequences[i].signals[j].timingBase64.length() * 2;
            }
        }
    }
    return total;
}

int Logger::getRecentTimingBytes() const {
    // 与 recentTimingBytes() 同一份口径（含 2× 安全系数），只是挪到 public —— 声明处的
    // "为什么门槛不能写死"写在 logger.h。这里不重复实现，免得两处漂移。
    return recentTimingBytes();
}

bool Logger::fitsTimingBudget(const SignalSequence& incoming) const {
    int add = 0;
    for (int j = 0; j < incoming.signalCount; j++) {
        if (incoming.signals[j].useTiming) {
            add += incoming.signals[j].timingBase64.length() * 2;
        }
    }
    return (recentTimingBytes() + add) <= RECENT_TIMING_BUDGET_BYTES;
}

void Logger::dropOldestSequence() {
    if (recentSequenceCount <= 0) return;

    // ⚠️⚠️ 不要用 recentSequenceIndex 定位"最旧"—— 我第一版就是这么写的，是错的：
    //   缓冲**未满**时 recentSequenceIndex 指向的是**还没写过的空槽**（下次要写的位置），
    //   清它等于白清，但 recentSequenceCount-- 真的减了 ⇒ 计数与实际内容脱节、
    //   读出去少一条、且再也补不回来。
    //   判据对照：getRecentSignalSequences 的读取起点是 0（未满）或 recentSequenceIndex（已满）。
    //
    // 稳妥做法：找 timestamp 最小的那条删。它是自洽的 —— 无论缓冲满不满、
    // 指针在哪，"最旧"定义上都成立。
    int oldest = -1;
    for (int i = 0; i < MAX_RECENT_SIGNALS; i++) {
        if (!recentSequences[i].valid) continue;
        if (oldest < 0 || recentSequences[i].timestamp < recentSequences[oldest].timestamp) {
            oldest = i;
        }
    }
    if (oldest < 0) {
        recentSequenceCount = 0;   // 没有任何有效条目（理论上不会发生）
        return;
    }
    recentSequences[oldest].clear();

    // ⚠️⚠️ 指针**一律不动**（v1.3.24 评审修正）。
    //   原来这里在"满环"时顺移一格，是错的：满环时 recentSequenceIndex 指的
    //   **就是最旧那一格**（下次要覆写的位置），删完它之后正确的写指针仍是这一格。
    //   顺移一格 ⇒ 下一写入盖掉**旁边那条活条目**，而被清空的格子变成永久空洞
    //   ⇒ 计数说 5 条、实际只有 4 条有效（getRecentSignalSequences 扫 valid 输出，
    //   所以外部看不出丢，只是容量悄悄降到 4/5）。
    //   不动指针时：满环删的就是写指针指的格子，下次正好写回去；未满时写指针
    //   本来就不指向最旧条目，更不该动。
    recentSequenceCount--;
}

int Logger::getLogCount() const {
    return log_count;
}

bool Logger::getLogs(LogEntry* entries, int max_count) {
    if (entries == nullptr || max_count <= 0) {
        return false;
    }
    
    int copy_count = (max_count < log_count) ? max_count : log_count;
    int start_idx = (log_count < MAX_LOG_ENTRIES) ? 0 : log_index;
    
    for (int i = 0; i < copy_count; i++) {
        int idx = (start_idx + i) % MAX_LOG_ENTRIES;
        if (logs[idx].valid) {
            entries[i] = logs[idx];
        }
    }
    
    return true;
}

void Logger::clearLogs() {
    for (int i = 0; i < MAX_LOG_ENTRIES; i++) {
        logs[i].valid = false;
        logs[i].type[0] = '\0';
        logs[i].content[0] = '\0';
    }
    log_index = 0;
    log_count = 0;
    // 清空之后文件内容就是 "[]"，直接删文件 —— 效果完全一样，但**零堆分配**。
    // 以前走 saveToFlash()，而它的 DynamicJsonDocument 下限是 2048 B
    // ⇒ "清空日志"这个动作在堆最紧张的瞬间反而去分配 2 KB 并写一次 flash
    // （HEALTH_SOFT_RECOVER 正是这么调的）。loadFromFlash() 本来就处理
    // "文件不存在"（记为空），所以删文件等价。
    LittleFS.remove(LOG_FILE);
    needsSave = false;
}

void Logger::clearRecentSequences() {
    // 本系统最大的一块可回收堆：recentSequences[5] × 最多 6 个信号，
    // 时序码信号的 timingBase64 是整段 Base64（200 个时序点 ≈ 534 字符），
    // 全是堆分配。logs[10] / currentSequence 里的才是定长 char 数组，
    // 清它们释放 0 字节 —— 所以软恢复必须先动这里。
    for (int i = 0; i < MAX_RECENT_SIGNALS; i++) {
        recentSequences[i].clear();
    }
    recentSequenceIndex = 0;
    recentSequenceCount = 0;
    currentSequence.clear();
    isBuildingSequence = false;
}

void Logger::setLogPublishCallback(LogPublishCallback callback) {
    logPublishCallback = callback;
}

void Logger::resetSignalInterval() {
    // 完成当前序列（如果有）
    if (isBuildingSequence) {
        finalizeCurrentSequence();
    }
    lastSignalTime = 0;
    isBuildingSequence = false;
    currentSequence.clear();
}

void Logger::publishToHA(const String& type, const String& content) {
    // 此函数由HA接口模块调用，这里只记录
    // 实际推送在 ha_mqtt_handler 中实现
}

void Logger::flush() {
    if (needsSave) {
        saveToFlash();
    }
}

void Logger::saveToFlash() {
    // 延迟保存：只在需要时保存
    if (!needsSave) {
        return;
    }
    
    // 内存保护：内存不足时跳过保存。门槛从 3072 降到 1024（v1.3.17）——
    // 配了 MQTT 后空闲堆常驻 3~5 KB，3072 会贴得太近；日志文件本身很小
    //（最多 10 条 × 约 88 B），真正要留的是 LittleFS 的写缓冲。
    if (ESP.getFreeHeap() < 1024) {
        Serial.println(F("[Logger] 内存不足，跳过保存日志到Flash"));
        needsSave = false;
        return;
    }
    
    File file = LittleFS.open(LOG_FILE, "w");
    if (!file) {
        return;
    }
    
    // 优化：更精确的JSON大小计算
    // 基础开销：数组结构约100字节
    // 每条日志：timestamp(8) + type字符串(实际长度) + content字符串(实际长度) + 对象开销(20)
    size_t jsonSize = 256; // 基础开销
    int start_idx = (log_count < MAX_LOG_ENTRIES) ? 0 : log_index;
    for (int i = 0; i < log_count; i++) {
        int idx = (start_idx + i) % MAX_LOG_ENTRIES;
        if (logs[idx].valid) {
            jsonSize += 28; // timestamp + 对象开销
            jsonSize += strlen(logs[idx].type);
            jsonSize += strlen(logs[idx].content);
        }
    }
    if (jsonSize < 2048) jsonSize = 2048;  // 最小2KB
    if (jsonSize > 6144) jsonSize = 6144;   // 限制最大6KB，避免内存不足
    
    // 检查是否有足够内存
    if (ESP.getFreeHeap() < jsonSize + 1024) {
        Serial.printf_P(PSTR("[Logger] 内存不足，无法保存日志 (需要: %d, 可用: %d)\n"), jsonSize, ESP.getFreeHeap());
        file.close();
        needsSave = false;
        return;
    }
    
    DynamicJsonDocument doc(jsonSize);
    JsonArray logArray = doc.to<JsonArray>();
    
    for (int i = 0; i < log_count; i++) {
        int idx = (start_idx + i) % MAX_LOG_ENTRIES;
        if (logs[idx].valid) {
            JsonObject entry = logArray.createNestedObject();
            entry["timestamp"] = logs[idx].timestamp;
            entry["type"] = logs[idx].type;
            entry["content"] = logs[idx].content;
        }
    }
    
    // 检查文档是否溢出
    if (doc.overflowed()) {
        Serial.printf_P(PSTR("[Logger: saveToFlash] JSON文档溢出，尝试使用更大的文档 (当前大小: %d bytes, 日志数: %d)\n"), jsonSize, log_count);
        // 尝试使用更大的文档重新构建
        size_t largerSize = jsonSize * 2;
        // 两道闸：① 不超过大小上限；② 当时**真的拿得出**这么多连续内存。
        // ② 以前完全没有 —— 只看"没超过 8KB"就去申请。而设备最缺内存的时刻恰恰
        // 就是日志最容易溢出的时刻（内容越长越占内存），于是重试必然失败，白折腾
        // 一趟还把堆搅得更碎。日志不是关键数据：宁可不重试，也不要在这里打穿堆。
        if (largerSize <= 8192 && ESP.getFreeHeap() > largerSize + 2048) {
            DynamicJsonDocument largerDoc(largerSize);
            JsonArray largerArray = largerDoc.to<JsonArray>();
            for (int i = 0; i < log_count; i++) {
                int idx = (start_idx + i) % MAX_LOG_ENTRIES;
                if (logs[idx].valid) {
                    JsonObject entry = largerArray.createNestedObject();
                    entry["timestamp"] = logs[idx].timestamp;
                    entry["type"] = logs[idx].type;
                    entry["content"] = logs[idx].content;
                }
            }
            if (!largerDoc.overflowed()) {
                // ⚠️ 以前这里是 `doc = largerDoc`（拷贝赋值）。ArduinoJson 的
                // BasicJsonDocument 拷贝赋值走 copyAssignFrom → reallocPool + set，
                // 会把内容**深拷贝**进一个新申请的等大内存池；realloc 若不能原地扩张，
                // 峰值 = 原池 + largerDoc + 新池 ≈ 4~5 倍。
                // move 赋值走 moveAssignFrom：**先 free 掉目标池**，再直接把
                // largerDoc 的池指针接过来 —— 不再申请、不再抄。
                // （依据：.pio/libdeps/esp12e/ArduinoJson/.../BasicJsonDocument.hpp:92,159）
                doc = std::move(largerDoc);
            } else {
                Serial.printf_P(PSTR("[Logger: saveToFlash] 更大的文档仍然溢出 (大小: %d bytes, 日志数: %d)\n"), largerSize, log_count);
            }
        } else {
            Serial.printf_P(PSTR("[Logger: saveToFlash] 内存不够，放弃扩容重试 (需要: %d bytes, 可用: %d)\n"), largerSize, ESP.getFreeHeap());
        }
    }
    
    serializeJson(doc, file);
    file.close();
    
    // 如果仍然溢出，记录错误
    if (doc.overflowed()) {
        Serial.printf_P(PSTR("[Logger: saveToFlash] JSON文档溢出，无法保存日志 (大小: %d bytes, 日志数: %d)\n"), jsonSize, log_count);
    }
    
    needsSave = false;
}

void Logger::loadFromFlash() {
    if (!LittleFS.exists(LOG_FILE)) {
        return;
    }
    
    File file = LittleFS.open(LOG_FILE, "r");
    if (!file) {
        return;
    }
    
    // 动态计算JSON大小
    size_t fileSize = file.size();
    size_t jsonSize = fileSize + 512;  // 加上解析开销
    if (jsonSize < 2048) jsonSize = 2048;
    DynamicJsonDocument doc(jsonSize);
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    if (error) {
        return;
    }
    
    JsonArray logArray = doc.as<JsonArray>();
    log_count = 0;
    
    for (JsonObject entry : logArray) {
        if (log_count >= MAX_LOG_ENTRIES) {
            break;
        }
        
        logs[log_count].timestamp = entry["timestamp"];
        const char* typeStr = entry["type"] | "";
        const char* contentStr = entry["content"] | "";
        strncpy(logs[log_count].type, typeStr, MAX_LOG_TYPE_LEN - 1);
        logs[log_count].type[MAX_LOG_TYPE_LEN - 1] = '\0';
        strncpy(logs[log_count].content, contentStr, MAX_LOG_CONTENT_LEN - 1);
        logs[log_count].content[MAX_LOG_CONTENT_LEN - 1] = '\0';
        logs[log_count].valid = true;
        log_count++;
    }
    
    log_index = log_count % MAX_LOG_ENTRIES;
    needsSave = false;
}

