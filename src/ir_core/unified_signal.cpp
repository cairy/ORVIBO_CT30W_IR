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
    
    // 从Base64解码（使用临时变量接收count，因为timingCount是int16_t）
    int decodedCount = timingCount;
    bool result = SignalUtils::base64ToTiming(timingBase64, timing, decodedCount, timingCount);
    if (result) {
        timingCount = (int16_t)decodedCount;
    }
    return result;
}

void SignalUtils::signalToJSON(const UnifiedSignal& signal, JsonObject& obj, bool hexFormat) {
    // 根据信号类型序列化不同字段
    obj["useTiming"] = signal.useTiming;
    obj["repeatCount"] = signal.repeatCount;
    
    if (signal.useTiming) {
        // 时序码信号
        obj["protocol"] = 0;
        obj["data"] = hexFormat ? "0x00000000" : (uint32_t)0;
        obj["timingCount"] = (int)signal.timingCount;
        obj["timingBase64"] = signal.timingBase64;
    } else {
        // 协议信号
        obj["protocol"] = signal.protocol;
        if (hexFormat) {
            obj["data"] = uint32ToHexString(signal.data);
        } else {
            obj["data"] = signal.data;
        }
        obj["timingCount"] = 0;
        obj["timingBase64"] = "";
    }
}

bool SignalUtils::signalFromJSON(JsonObject& obj, UnifiedSignal& signal) {
    signal.clear();
    
    // 兼容旧格式：如果存在 isProtocol 字段，根据它设置 useTiming
    // 新格式只使用 useTiming 字段
    if (obj.containsKey("isProtocol")) {
        bool isProtocol = obj["isProtocol"] | false;
        signal.useTiming = !isProtocol;
    } else {
        signal.useTiming = obj["useTiming"] | false;
    }
    signal.repeatCount = obj["repeatCount"] | 1;
    
    if (signal.useTiming) {
        // 时序码信号
        signal.timingCount = (int16_t)(obj["timingCount"] | 0);
        if (obj["timingBase64"].is<const char*>()) {
            signal.timingBase64 = obj["timingBase64"].as<const char*>();
        } else {
            signal.timingBase64 = "";
        }
    } else {
        // 协议信号
        signal.protocol = obj["protocol"] | 0;
        if (obj["data"].is<const char*>()) {
            signal.data = parseUint32FromHexString(obj["data"].as<const char*>());
        } else {
            signal.data = obj["data"] | 0;
        }
    }
    
    return signal.isValid();
}

bool SignalUtils::copySignal(const UnifiedSignal& src, UnifiedSignal& dst) {
    dst.clear();
    
    dst.useTiming = src.useTiming;
    dst.repeatCount = src.repeatCount;
    
    if (src.useTiming) {
        // 时序码信号
        dst.timingCount = src.timingCount;
        dst.timingBase64 = src.timingBase64;
        
        // 深拷贝时序码（如果已加载）
        if (src.timing != nullptr && src.timingCount > 0) {
            dst.timing = new IRTiming[src.timingCount];
            if (dst.timing == nullptr) {
                return false;
            }
            memcpy(dst.timing, src.timing, src.timingCount * sizeof(IRTiming));
        }
    } else {
        // 协议信号
        dst.protocol = src.protocol;
        dst.data = src.data;
    }
    
    return true;
}

// ============================================================================
// 时序码的**逐点容差**比较（v1.3.25）
// ============================================================================
//
// ⚠️ 为什么不能再用 Base64 精确匹配（这是本次故障的第二个根因）：
//   实测同一台艾美特风扇遥控器、同一个键，一次按键连发 7 帧，**帧帧不同**：
//     长标志 1290→1260→1240→1230→1210 µs（递减）
//     短间隔  430→ 450→ 480→ 490→ 510 µs（递增）
//   （占空比漂移 —— 整帧总周期不变，只是长/短各自挪；量化到 10µs 后逐字节比不上）
//   ⇒ 精确匹配下"设为 A 之后再按同一个键"永远认不出来 ⇒ **永远不转发**；
//     连发去重同样永远不生效。
//
// 判据：**同一路电平的漂移量 << 两种电平之间的间距**。
//   实测 短(350~520µs) vs 长(1210~1370µs)，跨类间距 ≥690µs，
//   而同类内最大差只有 80~90µs ⇒ 60µs 绝对容差 + 30% 相对容差能稳稳分开。
//
// 实现上**不解码成数组**：signalsEqual 会被 findRuleIndex / isSignalInMappings
// 在规则表上反复调用（热路径），另开两个 200 × 8B 的静态数组要多占 3.2 KB BSS。
// 改成按需取第 i 个点：Base64 每 4 字符解出 3 字节，点 i 占字节 2i、2i+1，
// 于是 O(1) 取点、零额外常驻内存。

// 取 Base64 解码后的第 k 个字节（k 从 0 起）
static bool b64RawByteAt(const String& s, int k, uint8_t& out) {
    if (k < 0) return false;
    const int group   = k / 3;      // 第几组 4 字符
    const int inGroup = k % 3;      // 组内第几个字节
    const int c0      = group * 4;
    if (c0 + 3 >= (int)s.length()) return false;   // 凑不满 4 个字符
    int v[4];
    for (int i = 0; i < 4; i++) {
        const char c = s[c0 + i];
        if      (c >= 'A' && c <= 'Z') v[i] = c - 'A';
        else if (c >= 'a' && c <= 'z') v[i] = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v[i] = c - '0' + 52;
        else if (c == '+')             v[i] = 62;
        else if (c == '/')             v[i] = 63;
        else                           return false;   // '=' 或脏字符
    }
    const uint32_t packed = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) |
                            ((uint32_t)v[2] <<  6) |  (uint32_t)v[3];
    out = (uint8_t)((packed >> (16 - inGroup * 8)) & 0xFF);
    return true;
}

// 取时序码第 idx 个点的时长（微秒）。编码见 timingToBase64：每点 2 字节小端 = 时长/10
static bool timingPointMicros(const String& s, int idx, unsigned long& us) {
    uint8_t lo = 0, hi = 0;
    if (!b64RawByteAt(s, idx * 2,     lo)) return false;
    if (!b64RawByteAt(s, idx * 2 + 1, hi)) return false;
    us = (unsigned long)(uint16_t)((uint16_t)lo | ((uint16_t)hi << 8)) * 10UL;
    return true;
}

bool SignalUtils::signalsEqual(const UnifiedSignal& a, const UnifiedSignal& b) {
    if (a.useTiming != b.useTiming) {
        return false;
    }
    
    if (a.useTiming) {
        // 点数不同 ⇒ 一定不是同一条信号（切帧后每条都是"一帧"，点数恒定）
        if (a.timingCount != b.timingCount) {
            return false;
        }
        // 快速路径：逐字节相同（同一份数据的副本走这里，省掉逐点解码）
        if (a.timingBase64 == b.timingBase64) {
            return true;
        }
        if (a.timingCount <= 0) {
            return true;   // 两边都是空时序码，点数已相等
        }
        // 逐点容差比较，判据见本文件上方 TIMING_MATCH_* 的说明
        for (int i = 0; i < a.timingCount; i++) {
            unsigned long x = 0, y = 0;
            if (!timingPointMicros(a.timingBase64, i, x)) return false;
            if (!timingPointMicros(b.timingBase64, i, y)) return false;
            const unsigned long diff = (x > y) ? (x - y) : (y - x);
            unsigned long tol = ((x > y) ? x : y) * TIMING_MATCH_REL_PERCENT / 100UL;
            if (tol < TIMING_MATCH_ABS_TOL_US) tol = TIMING_MATCH_ABS_TOL_US;
            if (diff > tol) return false;
        }
        return true;
    } else {
        // 使用协议：比较protocol和data
        return a.protocol == b.protocol && a.data == b.data;
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
        // ⚠️ 截断必须留痕：发射侧拿到被截断的波形 = 被控设备不认，而且以前毫无线索。
        Serial.printf_P(PSTR("[timing] 时序点 %d 超过上限 %d，已截断（base64 %u 字符）\n"),
                      timingCount, maxCount, (unsigned)base64.length());
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

void SignalUtils::sequenceToJSON(const SignalSequence& seq, JsonObject& obj, bool hexFormat) {
    // ⚠️ 辅助字段必须写在 signals / intervals **之前**（v1.3.24）
    // ArduinoJson 装不下时是**静默丢字段**，不报错。调用方（api_handlers.cpp 的
    // handleGetRecentSignals）给的 doc 容量按估算给，而"最近信号"里含时序码的
    // 一条就能吃掉 500+ 字节 ⇒ 5 条满载时必然装不下。
    // 以前 timestamp / matched 写在最后，于是容量一紧张就先丢它们 ——
    // 前端 app.js 的 formatTime(undefined) 直接返回空串，表现为
    // **"有的信号显示时间，有的不显示；刷新一次变一次"**（谁被丢取决于当次谁占位置）。
    // 提到最前面后，即使后面的 signals 被截断，这条记录的时间和匹配状态仍然可读。
    if (seq.timestamp > 0) {
        obj["timestamp"] = seq.timestamp;
    }
    if (seq.valid) {
        obj["matched"] = seq.matched;
    }

    // 信号数组
    JsonArray signalsArray = obj.createNestedArray("signals");
    for (int i = 0; i < seq.signalCount; i++) {
        JsonObject signalObj = signalsArray.createNestedObject();
        signalToJSON(seq.signals[i], signalObj, hexFormat);
    }
    
    // 间隔数组
    // 根据实际使用的间隔数量序列化（可能是 signalCount 或 signalCount-1）
    JsonArray intervalsArray = obj.createNestedArray("intervals");
    uint8_t intervalCount = seq.getIntervalCount();
    for (int i = 0; i < intervalCount; i++) {
        intervalsArray.add(seq.intervals[i]);
    }
    
    // 注意：序列不包含repeatCount，重复次数由外部控制
}

bool SignalUtils::sequenceFromJSON(JsonObject& obj, SignalSequence& seq) {
    seq.clear();
    
    // 解析信号数组
    if (!obj.containsKey("signals")) {
        return false;
    }
    JsonArray signalsArray = obj["signals"];
    if (signalsArray.isNull() || signalsArray.size() == 0) {
        return false;
    }
    
    int signalCount = 0;
    for (JsonObject signalObj : signalsArray) {
        if (signalCount >= MAX_B_SIGNALS_PER_RULE) {
            break;
        }
        if (!signalFromJSON(signalObj, seq.signals[signalCount])) {
            return false;
        }
        signalCount++;
    }
    
    if (signalCount == 0) {
        return false;
    }
    seq.signalCount = signalCount;
    
    // 解析间隔数组
    // 支持两种模式：intervals 数量等于 signalCount 或 signalCount-1
    if (obj.containsKey("intervals")) {
        JsonArray intervalsArray = obj["intervals"];
        int intervalCount = 0;
        for (uint16_t interval : intervalsArray) {
            if (intervalCount >= MAX_B_SIGNALS_PER_RULE) {
                break;
            }
            // 根据数量判断语义：
            // - 如果数量等于 signalCount：intervals[0] 是第一个信号前的间隔
            // - 如果数量等于 signalCount-1：intervals[i] 是 signals[i] 和 signals[i+1] 之间的间隔
            if (intervalCount < MAX_B_SIGNALS_PER_RULE) {
                seq.intervals[intervalCount] = interval;
            }
            intervalCount++;
        }
        // 如果间隔数量超过 signalCount，截断到 signalCount
        if (intervalCount > signalCount) {
            intervalCount = signalCount;
        }
    }
    
    // 注意：序列不包含repeatCount，重复次数由外部控制
    
    // 解析辅助字段（可选）
    seq.timestamp = obj["timestamp"] | 0;
    seq.matched = obj["matched"] | false;
    seq.valid = true;
    
    return seq.isValid();
}

bool SignalUtils::copySequence(const SignalSequence& src, SignalSequence& dst) {
    dst.clear();
    
    // 拷贝信号数组
    for (int i = 0; i < src.signalCount; i++) {
        if (!copySignal(src.signals[i], dst.signals[i])) {
            return false;
        }
    }
    dst.signalCount = src.signalCount;
    
    // 拷贝间隔数组（拷贝所有有效的间隔，最多 MAX_B_SIGNALS_PER_RULE 个）
    uint8_t intervalCount = src.getIntervalCount();
    for (int i = 0; i < intervalCount && i < MAX_B_SIGNALS_PER_RULE; i++) {
        dst.intervals[i] = src.intervals[i];
    }
    
    // 拷贝其他字段
    dst.timestamp = src.timestamp;
    dst.matched = src.matched;
    dst.valid = src.valid;
    
    return true;
}

bool SignalUtils::sequencesEqual(const SignalSequence& a, const SignalSequence& b) {
    if (a.signalCount != b.signalCount) {
        return false;
    }
    
    // 比较信号
    for (int i = 0; i < a.signalCount; i++) {
        if (!signalsEqual(a.signals[i], b.signals[i])) {
            return false;
        }
    }
    
    // 比较间隔（比较所有有效的间隔）
    uint8_t intervalCountA = a.getIntervalCount();
    uint8_t intervalCountB = b.getIntervalCount();
    if (intervalCountA != intervalCountB) {
        return false;
    }
    
    for (int i = 0; i < intervalCountA; i++) {
        if (a.intervals[i] != b.intervals[i]) {
            return false;
        }
    }
    
    return true;
}
