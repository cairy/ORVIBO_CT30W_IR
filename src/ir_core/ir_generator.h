#ifndef IR_GENERATOR_H
#define IR_GENERATOR_H

#include <Arduino.h>
#include "hardware/ir_receiver.h"

// 红外时序生成器
// 将协议数据（protocol + data）转换为时序数组（IRTiming）
class IRGenerator {
public:
    IRGenerator();
    bool init();
    
    // 生成时序数组（通用接口）
    // protocol: 协议类型（1=NEC, 2=RC5, 3=RC6, 4=Sony, 5=Samsung）
    // data: 协议数据
    // timing: 输出时序数组
    // count: 输出时序数量
    // maxCount: 时序数组最大容量
    bool generateTiming(uint32_t protocol, uint32_t data, IRTiming* timing, int& count, int maxCount);
    
    // 各协议时序生成函数
    bool generateNEC(uint32_t data, IRTiming* timing, int& count, int maxCount);
    bool generateRC5(uint32_t data, IRTiming* timing, int& count, int maxCount);
    bool generateRC6(uint32_t data, IRTiming* timing, int& count, int maxCount);
    bool generateSony(uint32_t data, IRTiming* timing, int& count, int maxCount);
    bool generateSamsung(uint32_t data, IRTiming* timing, int& count, int maxCount);

private:
    // 添加时序点
    // level: 电平（true=HIGH, false=LOW）
    // duration: 持续时间（微秒）
    bool addTiming(IRTiming* timing, int& count, int maxCount, bool level, unsigned long duration);
    
    // 添加载波时序（高电平）
    // duration: 持续时间（微秒）
    // freqHz: 载波频率（Hz），默认38kHz
    bool addCarrier(IRTiming* timing, int& count, int maxCount, unsigned long duration, uint32_t freqHz = 38000);
};

#endif // IR_GENERATOR_H

