#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>
#include "config/constants.h"
#include "ir_core/unified_signal.h"

// 日志条目结构（优化：使用char数组替代String以减少内存占用）
struct LogEntry {
    unsigned long timestamp;          // 时间戳
    char type[MAX_LOG_TYPE_LEN];     // 日志类型（receive/transmit/error/learn）
    char content[MAX_LOG_CONTENT_LEN]; // 日志内容
    bool valid;                       // 条目是否有效
};

// 日志管理器
class Logger {
public:
    // 日志推送回调函数类型（事件驱动推送）
    typedef void (*LogPublishCallback)(const char* type, const char* content);
    
    Logger();
    bool init();
    void log(const String& type, const String& content);
    void log(const char* type, const char* content);  // 优化：const char*版本，避免String对象创建
    void logReceive(const String& signal_info);
    void logReceiveSignal(const UnifiedSignal& signal, bool matched);  // 记录接收的信号（内部使用，自动检测序列）
    void logTransmit(const String& signal_info);
    void logError(const String& error_msg);
    void logLearn(const String& signal_info);
    int getLogCount() const;
    bool getLogs(LogEntry* entries, int max_count);
    int getRecentSignalSequences(SignalSequence* sequences, int max_count);  // 获取最近接收的信号序列
    // ⭐ v1.3.49：只读、**不分配**。返回环形缓冲里时序码 Base64 的总量
    //   （已含 recentTimingBytes() 那层 2× 多副本安全系数）。
    //   用途：`GET /api/signals/recent` 的内存门槛必须按**这一路实际要处理的量**定，
    //   不能再写死 —— 空列表时响应只有 65 字节，写死的 3072（要求 maxBlock ≥ 3584）
    //   会把 212（带 MQTT，空闲 maxBlock ~3120）的每一次请求都拒掉，
    //   用户看到的就是「设备内存不足，请稍后再试」。
    int getRecentTimingBytes() const;
    void clearLogs();
    // 释放"最近信号序列"里那几段堆分配的 Base64 —— 本系统最大的一块
    // 可回收堆，内存见底时的软恢复必须先动它（见 main.cpp 的 HEALTH_SOFT_RECOVER）。
    void clearRecentSequences();
    void publishToHA(const String& type, const String& content);
    void flush();  // 强制保存到Flash
    
    // 设置日志推送回调函数（由HA接口模块设置）
    void setLogPublishCallback(LogPublishCallback callback);

    // 重置信号间隔计时（供自动重置使用）
    void resetSignalInterval();

private:
    // 时序码堆预算（v1.3.24）—— 见 logger.cpp 的 finalizeCurrentSequence 注释。
    // ⚠️ 系数 2 是**刻意的安全余量**：同一份 Base64 在请求期间会在堆上存在多份
    //   （环形缓冲 / API 层深拷贝 / ArduinoJson 进 doc 时再复制）。
    //   ⛔ 旧注释写"String 是 UCS-2、每字符 2 字节"是**错的**（2026-10-03 查 core
    //   源码更正：ESP8266 的 String 是 char 数组，1 字节/字符）。
    // 实测 191 点 → 512 字符 → 按 1 KB/条计。环形缓冲 5 条 ⇒ 最坏 5 KB。
    //
    // ⚠️⚠️ 阈值必须按**配了 MQTT 的正式机**定，不能按开发机（v1.3.24 二次修正）：
    //   实测空闲堆 开发机 111 = 11712 B，正式机 212（有 MQTT）= **10592 B**（少 1120 B）。
    //   而单次 `GET /api/signals/recent` 会在此之上再要：doc 3072 + 响应 String ~2600，
    //   而且 getRecentSignalSequences 的 SignalSequence[5] 会把每条时序码
    //   **深拷贝一份**到 API 层（copySequence）⇒ 那 5 KB 在请求期间是**双份**。
    //   4096 预算下 212 的峰值会逼近 10 KB，只剩几百字节余量 ⇒ 一次浏览就可能 OOM。
    //   2048 = 2 条满帧时序码，212 上留出 ~3.5 KB 余量给 HTTP 与 MQTT。
    //   代价：满帧时序码只留 2 条；短帧（<100 点）能留 4~5 条，够排障用。
    static const int RECENT_TIMING_BUDGET_BYTES = 2048;

    int recentTimingBytes() const;
    bool fitsTimingBudget(const SignalSequence& incoming) const;
    void dropOldestSequence();
    // 时序码是否允许并入当前序列（时序码必须独占一条，见实现处注释）
    bool canJoinSequence(const UnifiedSignal& signal) const;

    LogEntry logs[MAX_LOG_ENTRIES];
    int log_index;
    int log_count;
    bool needsSave;                   // 是否需要保存到Flash（延迟保存）
    
    // 最近信号序列记录（环形缓冲区）
    SignalSequence recentSequences[MAX_RECENT_SIGNALS];
    int recentSequenceIndex;
    int recentSequenceCount;
    
    // 当前正在构建的序列（用于序列检测）
    SignalSequence currentSequence;
    bool isBuildingSequence;
    unsigned long lastSignalTime;  // 上一个信号的时间戳（用于计算时间间隔）
    bool lastSignalMatched;        // 上一个信号的匹配状态
    
    // 日志推送回调函数指针（事件驱动推送）
    LogPublishCallback logPublishCallback;
    
    // 完成当前序列构建（私有方法）
    void finalizeCurrentSequence();
    
    void saveToFlash();
    void loadFromFlash();
};

#endif // LOGGER_H

