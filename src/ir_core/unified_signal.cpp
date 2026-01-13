#include "unified_signal.h"
#include "utils/hex_utils.h"
#include "config/constants.h"
#include <ArduinoJson.h>

// Base64编码表
static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool UnifiedSignal::loadTiming() {
    if (!useTiming || timingBase64.length() == 0 || timingCount <= 0) {
        return false;
    }
    
    // 如果已经加载，直接返回
    if (timing != nullptr) {
        return true;
    }
    
    // 检查内存
    size_t requiredMemory = timingCount * sizeof(IRTiming) + 1024;
    if (ESP.getFreeHeap() < requiredMemory) {
        return false;
    }
    
    // 分配内存
    timing = new IRTiming[timingCount];
    if (timing == nullptr) {
        return false;
    }
    
    // 从Base64解码
    return SignalUtils::base64ToTiming(timingBase64, timing, timingCount, timingCount);
}

void SignalUtils::signalToJSON(const UnifiedSignal& signal, JsonObject& obj, bool hexFormat) {
    obj["protocol"] = signal.protocol;
    if (hexFormat) {
        obj["data"] = uint32ToHexString(signal.data);
    } else {
        obj["data"] = signal.data;
    }
    obj["isProtocol"] = signal.isProtocol;
    obj["useTiming"] = signal.useTiming;
    obj["timingCount"] = signal.timingCount;
    obj["repeatCount"] = signal.repeatCount;
    
    if (signal.useTiming && signal.timingBase64.length() > 0) {
        obj["timingBase64"] = signal.timingBase64;
    } else {
        obj["timingBase64"] = "";
    }
}

bool SignalUtils::signalFromJSON(JsonObject& obj, UnifiedSignal& signal) {
    signal.clear();
    
    signal.protocol = obj["protocol"] | 0;
    if (obj["data"].is<const char*>()) {
        signal.data = parseUint32FromHexString(obj["data"].as<const char*>());
    } else {
        signal.data = obj["data"] | 0;
    }
    signal.isProtocol = obj["isProtocol"] | false;
    signal.useTiming = obj["useTiming"] | false;
    signal.timingCount = obj["timingCount"] | 0;
    signal.repeatCount = obj["repeatCount"] | 1;
    
    if (signal.useTiming) {
        if (obj["timingBase64"].is<const char*>()) {
            signal.timingBase64 = obj["timingBase64"].as<const char*>();
        } else {
            signal.timingBase64 = "";
        }
    }
    
    return signal.isValid();
}

bool SignalUtils::copySignal(const UnifiedSignal& src, UnifiedSignal& dst) {
    dst.clear();
    
    dst.protocol = src.protocol;
    dst.data = src.data;
    dst.isProtocol = src.isProtocol;
    dst.useTiming = src.useTiming;
    dst.timingBase64 = src.timingBase64;
    dst.timingCount = src.timingCount;
    dst.repeatCount = src.repeatCount;
    
    // 深拷贝时序码（如果已加载）
    if (src.timing != nullptr && src.timingCount > 0) {
        dst.timing = new IRTiming[src.timingCount];
        if (dst.timing == nullptr) {
            return false;
        }
        memcpy(dst.timing, src.timing, src.timingCount * sizeof(IRTiming));
    }
    
    return true;
}

bool SignalUtils::signalsEqual(const UnifiedSignal& a, const UnifiedSignal& b) {
    if (a.useTiming != b.useTiming) {
        return false;
    }
    
    if (a.useTiming) {
        // 使用时序码：比较Base64字符串
        return a.timingBase64 == b.timingBase64 && a.timingCount == b.timingCount;
    } else {
        // 使用协议：比较protocol和data
        return a.isProtocol == b.isProtocol &&
               a.protocol == b.protocol &&
               a.data == b.data;
    }
}

String SignalUtils::timingToBase64(const IRTiming* timing, int count) {
    if (timing == nullptr || count == 0) {
        return "";
    }
    
    // 限制最大时序点数
    int maxCount = (count > MAX_LEARN_TIMING_COUNT) ? MAX_LEARN_TIMING_COUNT : count;
    int byteCount = maxCount * 2;  // 每个时序点2字节（持续时间）
    
    // 使用固定大小的栈缓冲区（400字节足够）
    if (byteCount > 400) {
        return "";  // 信号太长，无法处理
    }
    
    uint8_t bytes[400];  // 固定栈缓冲区
    
    // 将时序数据转换为字节数组（持续时间以10μs为单位）
    for (int i = 0; i < maxCount; i++) {
        uint16_t duration10us = (uint16_t)(timing[i].duration / 10);
        bytes[i * 2] = (uint8_t)(duration10us & 0xFF);
        bytes[i * 2 + 1] = (uint8_t)((duration10us >> 8) & 0xFF);
    }
    
    // Base64编码
    String base64;
    base64.reserve((byteCount * 4 / 3) + 4);  // 预分配空间
    
    int i = 0;
    int j = 0;
    uint8_t char_array_3[3];
    uint8_t char_array_4[4];
    
    while (i < byteCount) {
        char_array_3[j++] = bytes[i++];
        if (j == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            
            for (j = 0; j < 4; j++) {
                base64 += base64_chars[char_array_4[j]];
            }
            j = 0;
        }
    }
    
    // 处理剩余字节
    if (j > 0) {
        for (int k = j; k < 3; k++) {
            char_array_3[k] = '\0';
        }
        
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;
        
        for (int k = 0; k < j + 1; k++) {
            base64 += base64_chars[char_array_4[k]];
        }
        
        while (j++ < 3) {
            base64 += '=';
        }
    }
    
    return base64;
}

bool SignalUtils::base64ToTiming(const String& base64, IRTiming* timing, int& count, int maxCount) {
    if (base64.length() == 0 || timing == nullptr || maxCount <= 0) {
        count = 0;
        return false;
    }
    
    // Base64解码：计算解码后的字节数
    int paddingCount = 0;
    if (base64.length() > 0 && base64[base64.length() - 1] == '=') {
        paddingCount++;
        if (base64.length() > 1 && base64[base64.length() - 2] == '=') {
            paddingCount++;
        }
    }
    
    int base64Len = base64.length() - paddingCount;
    int byteCount = (base64Len * 3 / 4);
    
    // 检查解码后的字节数是否合理（每个时序点2字节）
    if (byteCount % 2 != 0 || byteCount == 0) {
        count = 0;
        return false;
    }
    
    int timingCount = byteCount / 2;
    if (timingCount > maxCount) {
        timingCount = maxCount;
        byteCount = timingCount * 2;
    }
    
    // 检查可用内存
    size_t requiredMemory = byteCount + 1024;
    if (ESP.getFreeHeap() < requiredMemory) {
        count = 0;
        return false;
    }
    
    // 根据数据大小选择栈缓冲区或动态分配
    uint8_t* bytes = nullptr;
    bool useStackBuffer = false;
    uint8_t stackBuffer[400];
    
    if (byteCount <= 400) {
        bytes = stackBuffer;
        useStackBuffer = true;
    } else {
        bytes = new uint8_t[byteCount];
        if (bytes == nullptr) {
            count = 0;
            return false;
        }
    }
    
    // Base64解码
    int i = 0;
    int j = 0;
    uint8_t char_array_4[4];
    uint8_t char_array_3[3];
    
    for (int idx = 0; idx < base64Len; idx++) {
        char c = base64[idx];
        if (c >= 'A' && c <= 'Z') {
            char_array_4[i++] = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            char_array_4[i++] = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            char_array_4[i++] = c - '0' + 52;
        } else if (c == '+') {
            char_array_4[i++] = 62;
        } else if (c == '/') {
            char_array_4[i++] = 63;
        } else if (c == '=') {
            break;
        }
        
        if (i == 4) {
            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
            
            for (i = 0; i < 3 && j < byteCount; i++) {
                bytes[j++] = char_array_3[i];
            }
            i = 0;
        }
    }
    
    // 处理剩余的Base64字符
    if (i > 0) {
        for (int k = i; k < 4; k++) {
            char_array_4[k] = 0;
        }
        
        char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
        char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
        
        for (int k = 0; k < i - 1 && j < byteCount; k++) {
            bytes[j++] = char_array_3[k];
        }
    }
    
    // 将字节数组转换为IRTiming数组
    count = timingCount;
    bool level = true;
    
    for (int idx = 0; idx < timingCount; idx++) {
        uint16_t duration10us = bytes[idx * 2] | (bytes[idx * 2 + 1] << 8);
        timing[idx].duration = duration10us * 10;
        timing[idx].level = level;
        level = !level;
    }
    
    // 释放动态分配的内存
    if (!useStackBuffer && bytes != nullptr) {
        delete[] bytes;
    }
    
    return (count > 0);
}
