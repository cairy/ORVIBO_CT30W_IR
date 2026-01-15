#ifndef UNIFIED_SIGNAL_H
#define UNIFIED_SIGNAL_H

#include <Arduino.h>
#include "hardware/ir_receiver.h"
#include <ArduinoJson.h>
#include "config/constants.h"

// 统一信号结构（替代SignalID和BSignal）
// 支持协议信号和时序码信号两种格式
// 使用联合体优化内存：协议信号和时序码信号共享存储空间
struct UnifiedSignal {
    // 信号类型标志
    bool useTiming;              // 是否使用时序码（true=时序码信号，false=协议信号）
    uint8_t repeatCount;         // 重复发送次数（默认1）
    
    // 使用联合体节省内存：协议信号和时序码信号不会同时使用
    union {
        // 协议信息（仅当 useTiming=false 时有效）
        struct {
            uint32_t protocol;   // 协议类型（1=NEC, 2=RC5, 3=RC6, 4=Sony, 5=Samsung, 0=未知）
            uint32_t data;       // 协议数据（32位）
        };
        // 时序码信息（仅当 useTiming=true 时有效）
        struct {
            int16_t timingCount; // 时序码数量（时序点的个数，最大32767足够）
            uint16_t _reserved;  // 保留字段，对齐用
        };
    };
    
    // 时序码Base64字符串（仅当 useTiming=true 时使用）
    // 注意：这个字段在协议信号时不使用，但由于String有默认构造函数，不会占用额外堆内存
    String timingBase64;
    
    // 运行时数据（不存储，仅运行时使用）
    IRTiming* timing;            // 时序码数据指针（运行时分配，转发时使用）
    
    // 构造函数
    UnifiedSignal() : useTiming(false), repeatCount(1), protocol(0), data(0), timing(nullptr) {}
    
    // 清理函数
    void clear() {
        if (useTiming) {
            timingBase64 = "";
            timingCount = 0;
        } else {
            protocol = 0;
            data = 0;
        }
        useTiming = false;
        freeTiming();
        repeatCount = 1;
    }
    
    // 是否为标准协议（辅助方法）
    bool isProtocol() const {
        return !useTiming;
    }
    
    // 验证函数
    bool isValid() const {
        if (useTiming) {
            return timingCount > 0 && timingBase64.length() > 0;
        } else {
            return protocol > 0 && protocol <= 5;
        }
    }
    
    // 是否有时序码数据
    bool hasTiming() const {
        return useTiming && timingCount > 0 && timingBase64.length() > 0;
    }
    
    // 从Base64加载时序码到内存
    bool loadTiming();
    
    // 释放时序码内存
    void freeTiming() {
        if (timing != nullptr) {
            delete[] timing;
            timing = nullptr;
        }
    }
    
    // 析构函数
    ~UnifiedSignal() {
        freeTiming();
    }
    
    // 拷贝构造函数（深拷贝）
    UnifiedSignal(const UnifiedSignal& other) : timing(nullptr) {
        useTiming = other.useTiming;
        repeatCount = other.repeatCount;
        if (useTiming) {
            timingCount = other.timingCount;
            timingBase64 = other.timingBase64;
        } else {
            protocol = other.protocol;
            data = other.data;
        }
        if (other.timing != nullptr && other.useTiming && other.timingCount > 0) {
            timing = new IRTiming[other.timingCount];
            if (timing != nullptr) {
                memcpy(timing, other.timing, other.timingCount * sizeof(IRTiming));
            }
        }
    }
    
    // 赋值运算符（深拷贝）
    UnifiedSignal& operator=(const UnifiedSignal& other) {
        if (this != &other) {
            freeTiming();
            useTiming = other.useTiming;
            repeatCount = other.repeatCount;
            if (useTiming) {
                timingCount = other.timingCount;
                timingBase64 = other.timingBase64;
            } else {
                protocol = other.protocol;
                data = other.data;
                timingBase64 = "";  // 清空时序码字符串
            }
            if (other.timing != nullptr && other.useTiming && other.timingCount > 0) {
                timing = new IRTiming[other.timingCount];
                if (timing != nullptr) {
                    memcpy(timing, other.timing, other.timingCount * sizeof(IRTiming));
                }
            }
        }
        return *this;
    }
};

// 统一信号序列结构（用于最近接收、学习、规则表、发送）
// 单个信号也是序列（signalCount=1，intervals=[] 或 intervals=[delay]）
// 注意：序列不包含repeatCount，重复次数由外部参数控制
struct SignalSequence {
    // 信号数组（最多5个信号）
    UnifiedSignal signals[MAX_B_SIGNALS_PER_RULE];
    
    // 信号数量（1-5）
    uint8_t signalCount;
    
    // 间隔数组（最多MAX_B_SIGNALS_PER_RULE个间隔）
    // 语义：
    // - 如果 intervals 数量等于 signalCount：intervals[0] 表示第一个信号前的间隔，intervals[i] (i>0) 表示 signals[i-1] 和 signals[i] 之间的间隔
    // - 如果 intervals 数量等于 signalCount-1：intervals[i] 表示 signals[i] 和 signals[i+1] 之间的间隔（向后兼容）
    uint16_t intervals[MAX_B_SIGNALS_PER_RULE];
    
    // 辅助字段（仅用于最近接收的信号）
    unsigned long timestamp;  // 时间戳（仅用于最近接收）
    bool matched;             // 是否在映射库中匹配（仅用于最近接收，仅第一个信号）
    bool valid;               // 是否有效
    
    // 构造函数
    SignalSequence() : signalCount(0), 
                      timestamp(0), matched(false), valid(false) {
        // 初始化信号数组
        for (int i = 0; i < MAX_B_SIGNALS_PER_RULE; i++) {
            signals[i].clear();
        }
        // 初始化间隔数组
        for (int i = 0; i < MAX_B_SIGNALS_PER_RULE; i++) {
            intervals[i] = 0;
        }
    }
    
    // 清理函数
    void clear() {
        for (int i = 0; i < MAX_B_SIGNALS_PER_RULE; i++) {
            signals[i].clear();
        }
        signalCount = 0;
        for (int i = 0; i < MAX_B_SIGNALS_PER_RULE; i++) {
            intervals[i] = 0;
        }
        timestamp = 0;
        matched = false;
        valid = false;
    }
    
    // 验证函数
    bool isValid() const {
        return valid && signalCount > 0 && signalCount <= MAX_B_SIGNALS_PER_RULE;
    }
    
    // 是否为单个信号（辅助方法）
    bool isSingle() const {
        return signalCount == 1;
    }
    
    // 获取第一个信号（辅助方法）
    const UnifiedSignal& getFirstSignal() const {
        return signals[0];
    }
    
    // 获取实际使用的间隔数量
    // 返回 signalCount 或 signalCount-1，取决于是否有第一个信号前的间隔
    // 判断逻辑：如果 intervals[signalCount-1] 有值，说明使用了 signalCount 个间隔（包括第一个信号前的间隔）
    uint8_t getIntervalCount() const {
        if (signalCount == 0) return 0;
        if (signalCount == 1) {
            // 单个信号：如果有间隔，说明是第一个信号前的间隔
            return (intervals[0] > 0) ? 1 : 0;
        }
        // 多个信号：检查最后一个间隔是否被使用
        // 如果 intervals[signalCount-1] 有值，说明使用了 signalCount 个间隔（包括第一个信号前的间隔）
        if (signalCount > 1 && intervals[signalCount - 1] > 0) {
            return signalCount;  // 有第一个信号前的间隔
        }
        return signalCount - 1;  // 标准模式：signalCount-1
    }
    
    // 添加信号到序列（返回是否成功）
    // interval: 间隔时间（毫秒）
    // isFirstInterval: 如果为true且signalCount==0，则interval表示第一个信号前的间隔
    bool addSignal(const UnifiedSignal& signal, uint16_t interval = 0, bool isFirstInterval = false) {
        if (signalCount >= MAX_B_SIGNALS_PER_RULE) {
            return false;  // 序列已满
        }
        
        // 添加信号（使用赋值运算符，UnifiedSignal 有深拷贝）
        signals[signalCount] = signal;
        
        // 处理间隔
        if (signalCount == 0) {
            // 第一个信号
            if (isFirstInterval) {
                // 有第一个信号前的间隔模式：intervals[0]是第一个信号前的间隔
                intervals[0] = interval;
            }
            // 如果isFirstInterval=false，不设置间隔
        } else {
            // 第二个及以后的信号
            if (isFirstInterval) {
                // 有第一个信号前的间隔模式：intervals[i] (i>0) 是 signals[i-1] 和 signals[i] 之间的间隔
                intervals[signalCount] = interval;
            } else {
                // 标准模式：intervals[i] 是 signals[i] 和 signals[i+1] 之间的间隔
                intervals[signalCount - 1] = interval;
            }
        }
        
        signalCount++;
        return true;
    }
};

// 统一信号工具类
class SignalUtils {
public:
    // 信号转JSON（统一方法）
    static void signalToJSON(const UnifiedSignal& signal, JsonObject& obj, bool hexFormat = true);
    
    // JSON转信号（统一方法）
    static bool signalFromJSON(JsonObject& obj, UnifiedSignal& signal);
    
    // 信号拷贝（深拷贝时序码）
    static bool copySignal(const UnifiedSignal& src, UnifiedSignal& dst);
    
    // 信号比较（用于匹配）
    static bool signalsEqual(const UnifiedSignal& a, const UnifiedSignal& b);
    
    // 时序码转Base64
    static String timingToBase64(const IRTiming* timing, int count);
    
    // Base64转时序码
    static bool base64ToTiming(const String& base64, IRTiming* timing, int& count, int maxCount);
    
    // 序列转JSON
    static void sequenceToJSON(const SignalSequence& seq, JsonObject& obj, bool hexFormat = true);
    
    // JSON转序列
    static bool sequenceFromJSON(JsonObject& obj, SignalSequence& seq);
    
    // 序列拷贝（深拷贝）
    static bool copySequence(const SignalSequence& src, SignalSequence& dst);
    
    // 序列比较（用于匹配）
    static bool sequencesEqual(const SignalSequence& a, const SignalSequence& b);
};

#endif // UNIFIED_SIGNAL_H
