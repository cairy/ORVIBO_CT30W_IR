#ifndef UNIFIED_SIGNAL_H
#define UNIFIED_SIGNAL_H

#include <Arduino.h>
#include "hardware/ir_receiver.h"
#include <ArduinoJson.h>
#include "config/constants.h"

// 统一信号结构（替代SignalID和BSignal）
// 支持协议信号和时序码信号两种格式
struct UnifiedSignal {
    // 协议信息（仅当 useTiming=false 时有效）
    uint32_t protocol;          // 协议类型（1=NEC, 2=RC5, 3=RC6, 4=Sony, 5=Samsung, 0=未知），仅当 useTiming=false 时有效
    uint32_t data;              // 协议数据（32位），仅当 useTiming=false 时有效
    
    // 时序码信息（仅当 useTiming=true 时有效）
    bool useTiming;              // 是否使用时序码（true=时序码信号，false=协议信号）
    String timingBase64;         // 时序码Base64字符串（存储用，Flash中），仅当 useTiming=true 时有效
    int timingCount;            // 时序码数量（时序点的个数），仅当 useTiming=true 时有效
    
    // 运行时数据（不存储，仅运行时使用）
    IRTiming* timing;           // 时序码数据指针（运行时分配，转发时使用）
    
    // 辅助字段
    uint8_t repeatCount;        // 重复发送次数（默认1），用于控制信号发送的重复次数
    
    // 构造函数
    UnifiedSignal() : protocol(0), data(0),
                      useTiming(false), timingCount(0), timing(nullptr), repeatCount(1) {}
    
    // 清理函数
    void clear() {
        protocol = 0;
        data = 0;
        useTiming = false;
        timingBase64 = "";
        timingCount = 0;
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
            return isProtocol() && protocol > 0 && protocol <= 5;
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
    UnifiedSignal(const UnifiedSignal& other) {
        protocol = other.protocol;
        data = other.data;
        useTiming = other.useTiming;
        timingBase64 = other.timingBase64;
        timingCount = other.timingCount;
        repeatCount = other.repeatCount;
        timing = nullptr;
        if (other.timing != nullptr && timingCount > 0) {
            timing = new IRTiming[timingCount];
            if (timing != nullptr) {
                memcpy(timing, other.timing, timingCount * sizeof(IRTiming));
            }
        }
    }
    
    // 赋值运算符（深拷贝）
    UnifiedSignal& operator=(const UnifiedSignal& other) {
        if (this != &other) {
            freeTiming();
            protocol = other.protocol;
            data = other.data;
            useTiming = other.useTiming;
            timingBase64 = other.timingBase64;
            timingCount = other.timingCount;
            repeatCount = other.repeatCount;
            timing = nullptr;
            if (other.timing != nullptr && timingCount > 0) {
                timing = new IRTiming[timingCount];
                if (timing != nullptr) {
                    memcpy(timing, other.timing, timingCount * sizeof(IRTiming));
                }
            }
        }
        return *this;
    }
};

// 统一信号序列结构（用于最近接收、学习、规则表、发送）
// 单个信号也是序列（signalCount=1，intervals=[]）
struct SignalSequence {
    // 信号数组（最多5个信号）
    UnifiedSignal signals[MAX_B_SIGNALS_PER_RULE];
    
    // 信号数量（1-5）
    uint8_t signalCount;
    
    // 间隔数组（signalCount-1个间隔，对应signalCount个信号）
    uint16_t intervals[MAX_B_SIGNALS_PER_RULE - 1];
    
    // 重复发送次数（默认1）
    uint8_t repeatCount;
    
    // 辅助字段（仅用于最近接收的信号）
    unsigned long timestamp;  // 时间戳（仅用于最近接收）
    bool matched;             // 是否在映射库中匹配（仅用于最近接收，仅第一个信号）
    bool valid;               // 是否有效
    
    // 构造函数
    SignalSequence() : signalCount(0), repeatCount(1), 
                      timestamp(0), matched(false), valid(false) {
        // 初始化信号数组
        for (int i = 0; i < MAX_B_SIGNALS_PER_RULE; i++) {
            signals[i].clear();
        }
        // 初始化间隔数组
        for (int i = 0; i < MAX_B_SIGNALS_PER_RULE - 1; i++) {
            intervals[i] = 0;
        }
    }
    
    // 清理函数
    void clear() {
        for (int i = 0; i < MAX_B_SIGNALS_PER_RULE; i++) {
            signals[i].clear();
        }
        signalCount = 0;
        for (int i = 0; i < MAX_B_SIGNALS_PER_RULE - 1; i++) {
            intervals[i] = 0;
        }
        repeatCount = 1;
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
    
    // 添加信号到序列（返回是否成功）
    bool addSignal(const UnifiedSignal& signal, uint16_t interval = 0) {
        if (signalCount >= MAX_B_SIGNALS_PER_RULE) {
            return false;  // 序列已满
        }
        
        // 添加信号（使用赋值运算符，UnifiedSignal 有深拷贝）
        signals[signalCount] = signal;
        signalCount++;
        
        // 添加间隔（如果有前一个信号）
        if (signalCount > 1) {
            intervals[signalCount - 2] = interval;
        }
        
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
