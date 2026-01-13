#ifndef UNIFIED_SIGNAL_H
#define UNIFIED_SIGNAL_H

#include <Arduino.h>
#include "hardware/ir_receiver.h"
#include <ArduinoJson.h>

// 统一信号结构（替代SignalID和BSignal）
// 支持协议信号和时序码信号两种格式
struct UnifiedSignal {
    // 协议信息（当useTiming=false时使用）
    uint32_t protocol;          // 协议类型（1=NEC, 2=RC5, 3=RC6, 4=Sony, 5=Samsung, 0=未知）
    uint32_t data;              // 协议数据（32位）
    bool isProtocol;            // 是否为标准协议（true=协议, false=时序码）
    
    // 时序码信息（当useTiming=true时使用）
    bool useTiming;              // 是否使用时序码（true=时序码, false=协议）
    String timingBase64;         // 时序码Base64字符串（存储用，Flash中）
    int timingCount;            // 时序码数量
    
    // 运行时数据（不存储，仅运行时使用）
    IRTiming* timing;           // 时序码数据指针（运行时分配，转发时使用）
    
    // 辅助字段
    uint8_t repeatCount;        // 重复次数（默认1）
    
    // 构造函数
    UnifiedSignal() : protocol(0), data(0), isProtocol(false),
                      useTiming(false), timingCount(0), timing(nullptr), repeatCount(1) {}
    
    // 清理函数
    void clear() {
        protocol = 0;
        data = 0;
        isProtocol = false;
        useTiming = false;
        timingBase64 = "";
        timingCount = 0;
        freeTiming();
        repeatCount = 1;
    }
    
    // 验证函数
    bool isValid() const {
        if (useTiming) {
            return timingCount > 0 && timingBase64.length() > 0;
        } else {
            return isProtocol && protocol > 0 && protocol <= 5;
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
        isProtocol = other.isProtocol;
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
            isProtocol = other.isProtocol;
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
};

#endif // UNIFIED_SIGNAL_H
