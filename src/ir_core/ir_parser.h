#ifndef IR_PARSER_H
#define IR_PARSER_H

#include <Arduino.h>
#include "hardware/ir_receiver.h"
#include "unified_signal.h"
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRutils.h>

// 红外信号解析器
class IRParser {
public:
    IRParser();
    ~IRParser();
    bool init();
    
    // 使用IRremoteESP8266库解码（推荐方法）
    bool decodeWithLibrary(UnifiedSignal& signal);
    
    // 禁用/启用库的接收（用于学习模式）
    void disableLibraryReceiver();
    void enableLibraryReceiver();
    
    // 使用自定义时序解析（用于学习模式）
    // 如果解析成功，设置protocol+data，useTiming=false
    // 如果解析失败，使用时序码，useTiming=true，保存timingBase64
    bool parseSignal(const IRTiming* timing, int count, UnifiedSignal& signal);
    
    String getProtocolName(uint32_t protocol) const;
    String signalToString(const UnifiedSignal& signal) const;

private:
    IRrecv* irrecv;  // IRremoteESP8266库的接收器对象
    
    // 将库的解码结果转换为UnifiedSignal
    bool convertDecodeResult(decode_results& results, UnifiedSignal& signal);
    
    // 自定义解析方法（保留用于学习模式）
    bool parseNEC(const IRTiming* timing, int count, UnifiedSignal& signal);
    bool parseRC5(const IRTiming* timing, int count, UnifiedSignal& signal);
    bool parseRC6(const IRTiming* timing, int count, UnifiedSignal& signal);
    bool parseSony(const IRTiming* timing, int count, UnifiedSignal& signal);
    bool parseSamsung(const IRTiming* timing, int count, UnifiedSignal& signal);
    bool isNoise(unsigned long duration) const;
};

#endif // IR_PARSER_H

