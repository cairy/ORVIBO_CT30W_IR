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

// 最近接收的信号结构
struct RecentSignal {
    UnifiedSignal signal;             // 统一信号
    unsigned long timestamp;           // 时间戳
    bool matched;                      // 是否在映射库中匹配
    bool valid;                        // 条目是否有效
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
    void logReceiveSignal(const UnifiedSignal& signal, bool matched);  // 记录接收的信号
    void logTransmit(const String& signal_info);
    void logError(const String& error_msg);
    void logLearn(const String& signal_info);
    int getLogCount() const;
    bool getLogs(LogEntry* entries, int max_count);
    int getRecentSignals(RecentSignal* signals, int max_count);  // 获取最近接收的信号
    void clearLogs();
    void publishToHA(const String& type, const String& content);
    void flush();  // 强制保存到Flash
    
    // 设置日志推送回调函数（由HA接口模块设置）
    void setLogPublishCallback(LogPublishCallback callback);

private:
    LogEntry logs[MAX_LOG_ENTRIES];
    int log_index;
    int log_count;
    bool needsSave;                   // 是否需要保存到Flash（延迟保存）
    
    // 最近信号记录（环形缓冲区）
    RecentSignal recentSignals[MAX_RECENT_SIGNALS];
    int recentSignalIndex;
    int recentSignalCount;
    
    // 上一个信号的时间戳（用于计算时间间隔）
    unsigned long lastSignalTime;
    
    // 日志推送回调函数指针（事件驱动推送）
    LogPublishCallback logPublishCallback;
    
    void saveToFlash();
    void loadFromFlash();
};

#endif // LOGGER_H

