#ifndef IR_PARSER_H
#define IR_PARSER_H

#include <Arduino.h>
#include "hardware/ir_receiver.h"
#include "unified_signal.h"
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRutils.h>

// 红外信号解析器
//
// 双解析器架构说明：
// 1. IRremoteESP8266 库解析器（decodeWithLibrary）：
//    - 用于正常工作模式（转发模式）
//    - 直接从 GPIO 中断接收并解码，效率高
//    - 支持的协议：NEC, RC5, RC6, Sony, Samsung 等
//
// 2. 手写时序解析器（parseSignal + parseNEC/RC5/...）：
//    - 用于学习模式（需要原始时序数据）
//    - 从 IRReceiver 的环形缓冲区读取原始时序
//    - 协议编号必须与库解析器保持一致（protocol: 1=NEC, 2=RC5, ...）
//    - 如果无法识别协议，保存为时序码（useTiming=true）
//
// 注意：两套解析器的协议编号映射必须同步维护
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

    // 丢弃当前采样、复位到"等下一个信号"（发射静默期里用来丢掉自发的余光）
    void resumeReceiver();
    
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

    // 库认不出协议时的兜底：把 results 里的原始时序落成 useTiming=true 的信号（v1.3.24）
    // decode() 返回 false 时 results.rawbuf / rawlen 仍然有效（IRrecv.cpp 在协议试错
    // **之前**就填好了它们），所以不需要碰 IRrecv 的私有全局。
    bool buildTimingFallback(const decode_results& results, UnifiedSignal& signal);

    // 自定义解析方法（保留用于学习模式）
    bool parseNEC(const IRTiming* timing, int count, UnifiedSignal& signal);
    bool parseRC5(const IRTiming* timing, int count, UnifiedSignal& signal);
    bool parseRC6(const IRTiming* timing, int count, UnifiedSignal& signal);
    bool parseSony(const IRTiming* timing, int count, UnifiedSignal& signal);
    bool parseSamsung(const IRTiming* timing, int count, UnifiedSignal& signal);
    bool isNoise(unsigned long duration) const;
};

#endif // IR_PARSER_H

