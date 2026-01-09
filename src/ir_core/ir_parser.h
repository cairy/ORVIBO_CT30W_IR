#ifndef IR_PARSER_H
#define IR_PARSER_H

#include <Arduino.h>
#include "hardware/ir_receiver.h"
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRutils.h>

// 信号标识结构
struct SignalID {
    uint32_t protocol;      // 协议类型（枚举值）
    uint32_t data;          // 协议数据
    uint32_t hash;          // 时序特征哈希（未匹配协议时使用）
    bool isProtocol;        // 是否为标准协议
};

// 红外信号解析器
class IRParser {
public:
    IRParser();
    ~IRParser();
    bool init();
    
    // 使用IRremoteESP8266库解码（推荐方法）
    bool decodeWithLibrary(SignalID& signalId);
    
    // 禁用/启用库的接收（用于学习模式）
    void disableLibraryReceiver();
    void enableLibraryReceiver();
    
    // 使用自定义时序解析（用于学习模式）
    bool parseSignal(const IRTiming* timing, int count, SignalID& signalId);
    
    String getProtocolName(uint32_t protocol) const;
    String signalToString(const SignalID& signalId) const;
    uint32_t calculateHash(const IRTiming* timing, int count) const;
    
    // 信号序列化/反序列化
    String signalToJSON(const SignalID& signalId) const;
    bool signalFromJSON(const String& json, SignalID& signalId);

private:
    IRrecv* irrecv;  // IRremoteESP8266库的接收器对象
    
    // 将库的解码结果转换为SignalID
    bool convertDecodeResult(decode_results& results, SignalID& signalId);
    
    // 自定义解析方法（保留用于学习模式）
    bool parseNEC(const IRTiming* timing, int count, SignalID& signalId);
    bool parseRC5(const IRTiming* timing, int count, SignalID& signalId);
    bool parseRC6(const IRTiming* timing, int count, SignalID& signalId);
    bool parseSony(const IRTiming* timing, int count, SignalID& signalId);
    bool parseSamsung(const IRTiming* timing, int count, SignalID& signalId);
    bool isNoise(unsigned long duration) const;
};

#endif // IR_PARSER_H

