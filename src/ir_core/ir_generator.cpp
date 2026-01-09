#include "ir_generator.h"

IRGenerator::IRGenerator() {
}

bool IRGenerator::init() {
    return true;
}

bool IRGenerator::generateTiming(uint32_t protocol, uint32_t data, IRTiming* timing, int& count, int maxCount) {
    if (timing == nullptr || maxCount <= 0) {
        count = 0;
        return false;
    }
    
    count = 0;
    
    switch (protocol) {
        case 1:  // NEC
            return generateNEC(data, timing, count, maxCount);
        case 2:  // RC5
            return generateRC5(data, timing, count, maxCount);
        case 3:  // RC6
            return generateRC6(data, timing, count, maxCount);
        case 4:  // Sony
            return generateSony(data, timing, count, maxCount);
        case 5:  // Samsung
            return generateSamsung(data, timing, count, maxCount);
        default:
            count = 0;
            return false;
    }
}

bool IRGenerator::addTiming(IRTiming* timing, int& count, int maxCount, bool level, unsigned long duration) {
    if (timing == nullptr || count >= maxCount) {
        return false;
    }
    
    timing[count].level = level;
    timing[count].duration = duration;
    count++;
    return true;
}

bool IRGenerator::addCarrier(IRTiming* timing, int& count, int maxCount, unsigned long duration, uint32_t freqHz) {
    // 载波就是高电平
    return addTiming(timing, count, maxCount, true, duration);
}

bool IRGenerator::generateNEC(uint32_t data, IRTiming* timing, int& count, int maxCount) {
    // NEC协议时序生成
    // 引导码：9000μs高 + 4500μs低
    // 数据位：560μs低 + 载波（0: 560μs, 1: 1680μs）
    // 结束位：560μs低
    
    count = 0;
    
    // 引导码：9000μs高
    if (!addCarrier(timing, count, maxCount, 9000)) return false;
    
    // 引导码：4500μs低
    if (!addTiming(timing, count, maxCount, false, 4500)) return false;
    
    // 32位数据
    for (int bit = 0; bit < 32; bit++) {
        // 560μs低电平
        if (!addTiming(timing, count, maxCount, false, 560)) return false;
        
        // 载波时间：根据数据位决定
        bool bitValue = (data >> bit) & 1;
        if (bitValue) {
            // 逻辑1：1680μs载波
            if (!addCarrier(timing, count, maxCount, 1680)) return false;
        } else {
            // 逻辑0：560μs载波
            if (!addCarrier(timing, count, maxCount, 560)) return false;
        }
    }
    
    // 结束位：560μs低
    if (!addTiming(timing, count, maxCount, false, 560)) return false;
    
    return true;
}

bool IRGenerator::generateRC5(uint32_t data, IRTiming* timing, int& count, int maxCount) {
    // RC5协议时序生成
    // 起始位：2个位时间（3556μs）
    // 数据位：曼彻斯特编码，位中间跳变
    // 位时间：1778μs
    
    count = 0;
    
    // 起始位：2个位时间（3556μs）高电平
    if (!addCarrier(timing, count, maxCount, 3556)) return false;
    
    // 13位数据（1位控制位 + 5位地址 + 6位命令）
    unsigned long bitTime = 1778;  // 1.778ms
    
    for (int bit = 12; bit >= 0; bit--) {  // 从高位开始
        bool bitValue = (data >> bit) & 1;
        
        if (bitValue) {
            // 逻辑1：前半部分高电平，后半部分低电平
            if (!addCarrier(timing, count, maxCount, bitTime / 2)) return false;
            if (!addTiming(timing, count, maxCount, false, bitTime / 2)) return false;
        } else {
            // 逻辑0：前半部分低电平，后半部分高电平
            if (!addTiming(timing, count, maxCount, false, bitTime / 2)) return false;
            if (!addCarrier(timing, count, maxCount, bitTime / 2)) return false;
        }
    }
    
    return true;
}

bool IRGenerator::generateRC6(uint32_t data, IRTiming* timing, int& count, int maxCount) {
    // RC6协议时序生成
    // 前导码：6000μs高 + 2000μs低 + 266μs高 + 266μs低
    // 模式位：444μs高 + 444μs低
    // 数据位：444μs高 + 444μs低（0）或888μs高 + 444μs低（1）
    
    count = 0;
    
    // 前导码：6000μs高
    if (!addCarrier(timing, count, maxCount, 6000)) return false;
    
    // 2000μs低
    if (!addTiming(timing, count, maxCount, false, 2000)) return false;
    
    // 266μs高
    if (!addCarrier(timing, count, maxCount, 266)) return false;
    
    // 266μs低
    if (!addTiming(timing, count, maxCount, false, 266)) return false;
    
    // 模式位：444μs高（默认模式0）
    if (!addCarrier(timing, count, maxCount, 444)) return false;
    
    // 模式位低电平：444μs
    if (!addTiming(timing, count, maxCount, false, 444)) return false;
    
    // 20位数据
    for (int bit = 19; bit >= 0; bit--) {  // 从高位开始
        bool bitValue = (data >> bit) & 1;
        
        if (bitValue) {
            // 逻辑1：888μs高
            if (!addCarrier(timing, count, maxCount, 888)) return false;
        } else {
            // 逻辑0：444μs高
            if (!addCarrier(timing, count, maxCount, 444)) return false;
        }
        
        // 低电平间隔：444μs
        if (!addTiming(timing, count, maxCount, false, 444)) return false;
    }
    
    return true;
}

bool IRGenerator::generateSony(uint32_t data, IRTiming* timing, int& count, int maxCount) {
    // Sony协议时序生成
    // 引导码：根据数据位数选择（12位: 2400μs, 15/20位: 4800μs）
    // 数据位：1200μs高 + 600μs低（1）或600μs高 + 600μs低（0）
    
    count = 0;
    
    // 判断数据位数（从data的最高位判断，或使用固定12位）
    // 这里假设使用12位格式（最常见）
    int dataBits = 12;
    if (data > 0xFFF) {
        if (data > 0xFFFF) {
            dataBits = 20;
        } else {
            dataBits = 15;
        }
    }
    
    // 引导码高电平
    unsigned long headerTime = (dataBits == 12) ? 2400 : 4800;
    if (!addCarrier(timing, count, maxCount, headerTime)) return false;
    
    // 引导码低电平：600μs
    if (!addTiming(timing, count, maxCount, false, 600)) return false;
    
    // 数据位
    for (int bit = dataBits - 1; bit >= 0; bit--) {  // 从高位开始
        bool bitValue = (data >> bit) & 1;
        
        if (bitValue) {
            // 逻辑1：1200μs高 + 600μs低
            if (!addCarrier(timing, count, maxCount, 1200)) return false;
        } else {
            // 逻辑0：600μs高 + 600μs低
            if (!addCarrier(timing, count, maxCount, 600)) return false;
        }
        
        // 低电平：600μs
        if (!addTiming(timing, count, maxCount, false, 600)) return false;
    }
    
    return true;
}

bool IRGenerator::generateSamsung(uint32_t data, IRTiming* timing, int& count, int maxCount) {
    // Samsung协议时序生成
    // 引导码：4500μs高 + 4500μs低
    // 数据位：560μs低 + 载波（0: 560μs, 1: 1680μs）
    
    count = 0;
    
    // 引导码：4500μs高
    if (!addCarrier(timing, count, maxCount, 4500)) return false;
    
    // 引导码：4500μs低
    if (!addTiming(timing, count, maxCount, false, 4500)) return false;
    
    // 32位数据
    for (int bit = 0; bit < 32; bit++) {
        // 560μs低电平
        if (!addTiming(timing, count, maxCount, false, 560)) return false;
        
        // 载波时间：根据数据位决定
        bool bitValue = (data >> bit) & 1;
        if (bitValue) {
            // 逻辑1：1680μs载波
            if (!addCarrier(timing, count, maxCount, 1680)) return false;
        } else {
            // 逻辑0：560μs载波
            if (!addCarrier(timing, count, maxCount, 560)) return false;
        }
    }
    
    return true;
}

