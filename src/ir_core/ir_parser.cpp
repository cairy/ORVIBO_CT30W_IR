#include "ir_parser.h"
#include "config/pins.h"
#include <ArduinoJson.h>

IRParser::IRParser() : irrecv(nullptr) {
}

IRParser::~IRParser() {
    if (irrecv != nullptr) {
        delete irrecv;
        irrecv = nullptr;
    }
}

bool IRParser::init() {
    // 初始化IRremoteESP8266库的接收器
    if (irrecv == nullptr) {
        irrecv = new IRrecv(IR_RX_PIN);
        if (irrecv == nullptr) {
            return false;
        }
    }
    
    // 启动接收器
    irrecv->enableIRIn();
    
    return true;
}

bool IRParser::decodeWithLibrary(SignalID& signalId) {
    if (irrecv == nullptr) {
        return false;
    }
    
    decode_results results;
    
    // 尝试解码
    if (!irrecv->decode(&results)) {
        return false;
    }
    
    // 转换为SignalID
    bool success = convertDecodeResult(results, signalId);
    
    // 调试输出（可选，通过宏控制）
    #ifdef DEBUG_IR_DECODE
    Serial.print("[IR解码] 协议类型: ");
    Serial.print((int)results.decode_type);
    Serial.print(", 值: 0x");
    Serial.print(results.value, HEX);
    Serial.print(", 转换后: ");
    Serial.println(signalToString(signalId));
    #endif
    
    // 准备接收下一个信号
    irrecv->resume();
    
    return success;
}

void IRParser::disableLibraryReceiver() {
    if (irrecv != nullptr) {
        // 禁用中断处理程序，重新注册自定义中断处理程序
        detachInterrupt(digitalPinToInterrupt(IR_RX_PIN));
    }
}

void IRParser::enableLibraryReceiver() {
    if (irrecv != nullptr) {
        // 重新启用库的接收器
        irrecv->enableIRIn();
    }
}

bool IRParser::convertDecodeResult(decode_results& results, SignalID& signalId) {
    // 初始化SignalID
    signalId.protocol = 0;
    signalId.data = 0;
    signalId.hash = 0;
    signalId.isProtocol = false;
    
    // 检查是否有有效数据
    // IRremoteESP8266库中，UNKNOWN通常值为-1
    if (results.decode_type == UNKNOWN) {
        // 未知协议，使用原始数据计算哈希
        signalId.isProtocol = false;
        signalId.hash = (uint32_t)results.value;
        return true;
    }
    
    // 已知协议，转换为我们的协议编号
    signalId.isProtocol = true;
    signalId.data = (uint32_t)results.value;
    
    // 协议类型映射（使用库的枚举值）
    // IRremoteESP8266库使用decode_type_t枚举，直接比较枚举值
    decode_type_t decodeType = results.decode_type;
    
    // 根据协议类型映射到我们的协议编号
    // 只使用库中确实存在的枚举值
    if (decodeType == NEC || decodeType == NEC_LIKE) {
        signalId.protocol = 1;  // NEC
    }
    else if (decodeType == RC5 || decodeType == RC5X) {
        signalId.protocol = 2;  // RC5
    }
    else if (decodeType == RC6) {
        signalId.protocol = 3;  // RC6
    }
    else if (decodeType == SONY) {
        signalId.protocol = 4;  // Sony
    }
    else if (decodeType == SAMSUNG || decodeType == SAMSUNG36) {
        signalId.protocol = 5;  // Samsung
    }
    // 其他协议，使用哈希
    else {
        signalId.isProtocol = false;
        signalId.protocol = 0;
        signalId.hash = (uint32_t)results.value;
    }
    
    return true;
}

bool IRParser::parseSignal(const IRTiming* timing, int count, SignalID& signalId) {
    if (timing == nullptr || count <= 0) {
        return false;
    }
    
    // 过滤噪声（<50μs的脉冲）
    int validCount = 0;
    for (int i = 0; i < count; i++) {
        if (!isNoise(timing[i].duration)) {
            validCount++;
        }
    }
    
    if (validCount < 10) {
        return false;  // 信号太短，可能是噪声
    }
    
    // 尝试解析各种协议
    if (parseNEC(timing, count, signalId)) {
        signalId.isProtocol = true;
        return true;
    }
    if (parseRC5(timing, count, signalId)) {
        signalId.isProtocol = true;
        return true;
    }
    if (parseRC6(timing, count, signalId)) {
        signalId.isProtocol = true;
        return true;
    }
    if (parseSony(timing, count, signalId)) {
        signalId.isProtocol = true;
        return true;
    }
    if (parseSamsung(timing, count, signalId)) {
        signalId.isProtocol = true;
        return true;
    }
    
    // 未匹配协议，使用时序特征哈希
    signalId.isProtocol = false;
    signalId.hash = calculateHash(timing, count);
    signalId.protocol = 0;
    signalId.data = 0;
    return true;
}

String IRParser::getProtocolName(uint32_t protocol) const {
    // 协议名称映射
    switch (protocol) {
        case 1: return "NEC";
        case 2: return "RC5";
        case 3: return "RC6";
        case 4: return "Sony";
        case 5: return "Samsung";
        default: return "Unknown";
    }
}

String IRParser::signalToString(const SignalID& signalId) const {
    if (signalId.isProtocol) {
        return getProtocolName(signalId.protocol) + ":" + String(signalId.data, HEX);
    } else {
        return "Hash:" + String(signalId.hash, HEX);
    }
}

uint32_t IRParser::calculateHash(const IRTiming* timing, int count) const {
    // 简单的哈希算法：基于时序特征
    uint32_t hash = 0;
    for (int i = 0; i < count && i < 50; i++) {  // 只使用前50个点
        hash = hash * 31 + (timing[i].duration / 10);  // 量化到10μs
        hash = hash * 31 + (timing[i].level ? 1 : 0);
    }
    return hash;
}

bool IRParser::parseNEC(const IRTiming* timing, int count, SignalID& signalId) {
    // NEC协议解析
    // NEC协议格式：引导码(9ms高+4.5ms低) + 32位数据
    // 标准格式：地址(8位) + 地址反码(8位) + 命令(8位) + 命令反码(8位)
    // 扩展格式：地址(16位) + 命令(16位)
    // 重复码：9ms高 + 2.25ms低 + 560μs低（无数据）
    
    if (count < 10) return false;  // 至少需要引导码
    
    int idx = 0;
    
    // 检查引导码：第一个应该是高电平，持续时间约9ms（容差±20%）
    if (!timing[idx].level || timing[idx].duration < 7200 || timing[idx].duration > 10800) {
        return false;
    }
    idx++;
    
    if (idx >= count) return false;
    
    // 检查是否为重复码（引导码后低电平约2.25ms）
    if (!timing[idx].level && timing[idx].duration >= 1800 && timing[idx].duration <= 2700) {
        // 重复码：9ms高 + 2.25ms低 + 560μs低
        idx++;
        if (idx >= count) return false;
        if (timing[idx].level || timing[idx].duration < 400 || timing[idx].duration > 700) {
            return false;
        }
        // 重复码使用上一次的数据（这里返回false，由上层处理）
        return false;
    }
    
    // 引导码的低电平部分，约4.5ms（容差±20%）
    if (timing[idx].level || timing[idx].duration < 3600 || timing[idx].duration > 5400) {
        return false;
    }
    idx++;
    
    // 解析32位数据
    if (count < idx + 64) return false;  // 至少需要64个时序点（32位*2）
    
    uint32_t data = 0;
    for (int bit = 0; bit < 32; bit++) {
        if (idx >= count - 1) {
            return false;  // 数据不足
        }
        
        // 每个位由560μs低电平 + 载波时间组成
        // 逻辑0: 560μs载波（容差±20%）
        // 逻辑1: 1680μs载波（容差±20%）
        if (timing[idx].level) {
            return false;  // 应该是低电平
        }
        
        // 放宽容差范围到±20%
        if (timing[idx].duration < 448 || timing[idx].duration > 672) {
            return false;  // 低电平持续时间应该在560μs左右
        }
        idx++;
        
        if (idx >= count) {
            return false;
        }
        
        // 检查载波时间判断是0还是1
        if (!timing[idx].level) {
            return false;  // 应该是高电平（载波）
        }
        
        unsigned long carrierTime = timing[idx].duration;
        // 放宽容差范围到±20%
        if (carrierTime > 1344) {  // 1680 * 0.8 = 1344
            // 逻辑1：载波时间约1680μs
            data |= (1UL << bit);
        } else if (carrierTime > 448) {  // 560 * 0.8 = 448
            // 逻辑0：载波时间约560μs
            // data位保持0
        } else {
            return false;  // 载波时间异常
        }
        idx++;
    }
    
    // NEC协议使用32位数据：地址(8位) + 地址反码(8位) + 命令(8位) + 命令反码(8位)
    // 或者扩展格式：地址(16位) + 命令(16位)
    // 这里我们使用完整的32位数据作为信号标识
    signalId.protocol = 1;  // NEC
    signalId.data = data;  // 使用完整的32位数据
    return true;
}

bool IRParser::parseRC5(const IRTiming* timing, int count, SignalID& signalId) {
    // RC5协议解析
    // 载波频率：36kHz
    // 位时间：1.778ms（64个载波周期）
    // 曼彻斯特编码：位中间跳变表示逻辑值
    // 数据格式：1位起始位(2个位时间) + 1位控制位 + 5位地址 + 6位命令（共13位）
    
    if (count < 30) return false;  // RC5至少需要30个时序点（13位*2 + 起始位）
    
    int idx = 0;
    
    // 检测起始位：2个位时间（约3.556ms）
    // 起始位应该是高电平或低电平，持续2个位时间
    unsigned long startBitDuration = 0;
    bool startLevel = timing[idx].level;
    
    // 跳过起始位的前半部分
    while (idx < count && timing[idx].level == startLevel) {
        startBitDuration += timing[idx].duration;
        idx++;
    }
    
    // 起始位应该是2个位时间（1.778ms * 2 = 3.556ms，容差±20%）
    if (startBitDuration < 2845 || startBitDuration > 4267) {
        return false;
    }
    
    // 解析13位数据（1位控制位 + 5位地址 + 6位命令）
    uint32_t data = 0;
    
    for (int bit = 0; bit < 13; bit++) {
        if (idx >= count - 1) {
            return false;
        }
        
        // 曼彻斯特编码：位中间必须有跳变
        // 逻辑0：前半部分低电平，后半部分高电平（或相反）
        // 逻辑1：前半部分高电平，后半部分低电平（或相反）
        
        unsigned long firstHalf = 0;
        unsigned long secondHalf = 0;
        bool firstLevel = timing[idx].level;
        
        // 读取前半部分
        while (idx < count && timing[idx].level == firstLevel) {
            firstHalf += timing[idx].duration;
            idx++;
        }
        
        if (idx >= count) return false;
        
        // 读取后半部分
        bool secondLevel = timing[idx].level;
        while (idx < count && timing[idx].level == secondLevel) {
            secondHalf += timing[idx].duration;
            idx++;
        }
        
        // 检查是否有跳变（前后电平应该不同）
        if (firstLevel == secondLevel) {
            return false;  // 曼彻斯特编码必须有跳变
        }
        
        // 检查位时间（容差±20%）
        unsigned long totalTime = firstHalf + secondHalf;
        if (totalTime < 1422 || totalTime > 2134) {
            return false;
        }
        
        // 判断逻辑值：根据跳变方向
        // 如果前半部分时间更长，可能是逻辑0或1，需要根据实际协议判断
        // RC5协议：如果前半部分是低电平，则为逻辑0；如果前半部分是高电平，则为逻辑1
        if (firstLevel == false) {  // 前半部分是低电平
            // 逻辑0
            // data位保持0
        } else {  // 前半部分是高电平
            // 逻辑1
            data |= (1UL << (12 - bit));  // 从高位开始
        }
    }
    
    signalId.protocol = 2;  // RC5
    signalId.data = data;
    return true;
}

bool IRParser::parseRC6(const IRTiming* timing, int count, SignalID& signalId) {
    // RC6协议解析
    // 载波频率：36kHz
    // 前导码：6ms高 + 2ms低 + 266μs高 + 266μs低
    // 位时间：444μs（16个载波周期）
    // 数据格式：前导码 + 模式位 + 20位数据
    
    if (count < 50) return false;  // RC6至少需要50个时序点
    
    int idx = 0;
    
    // 检测前导码：6ms高 + 2ms低 + 266μs高 + 266μs低（容差±20%）
    // 6ms高电平
    if (!timing[idx].level || timing[idx].duration < 4800 || timing[idx].duration > 7200) {
        return false;
    }
    idx++;
    
    if (idx >= count) return false;
    
    // 2ms低电平
    if (timing[idx].level || timing[idx].duration < 1600 || timing[idx].duration > 2400) {
        return false;
    }
    idx++;
    
    if (idx >= count) return false;
    
    // 266μs高电平
    if (!timing[idx].level || timing[idx].duration < 213 || timing[idx].duration > 319) {
        return false;
    }
    idx++;
    
    if (idx >= count) return false;
    
    // 266μs低电平
    if (timing[idx].level || timing[idx].duration < 213 || timing[idx].duration > 319) {
        return false;
    }
    idx++;
    
    // 解析模式位（1位，444μs）
    if (idx >= count - 1) return false;
    
    unsigned long modeBitTime = 0;
    bool modeLevel = timing[idx].level;
    while (idx < count && timing[idx].level == modeLevel) {
        modeBitTime += timing[idx].duration;
        idx++;
    }
    
    // 模式位时间应该是444μs（容差±20%）
    if (modeBitTime < 355 || modeBitTime > 533) {
        return false;
    }
    
    // 解析20位数据
    uint32_t data = 0;
    
    for (int bit = 0; bit < 20; bit++) {
        if (idx >= count) {
            return false;
        }
        
        // RC6使用脉宽编码：短脉冲(444μs)表示0，长脉冲(888μs)表示1
        unsigned long pulseTime = 0;
        bool pulseLevel = timing[idx].level;
        
        while (idx < count && timing[idx].level == pulseLevel) {
            pulseTime += timing[idx].duration;
            idx++;
        }
        
        // 判断逻辑值
        if (pulseTime >= 710) {  // 888 * 0.8 = 710
            // 长脉冲：逻辑1
            data |= (1UL << (19 - bit));  // 从高位开始
        } else if (pulseTime >= 355) {  // 444 * 0.8 = 355
            // 短脉冲：逻辑0
            // data位保持0
        } else {
            return false;  // 脉冲时间异常
        }
        
        // 跳过低电平间隔
        if (idx < count && !timing[idx].level) {
            idx++;
        }
    }
    
    signalId.protocol = 3;  // RC6
    signalId.data = data;
    return true;
}

bool IRParser::parseSony(const IRTiming* timing, int count, SignalID& signalId) {
    // Sony协议解析
    // 载波频率：40kHz
    // 引导码：2400μs高 + 600μs低（12位）或4800μs高 + 600μs低（15/20位）
    // 数据位：1200μs高 + 600μs低（1）或600μs高 + 600μs低（0）
    // 数据格式：12/15/20位
    
    if (count < 20) return false;
    
    int idx = 0;
    
    // 检测引导码，确定数据位数
    if (!timing[idx].level) {
        return false;
    }
    
    unsigned long headerHigh = timing[idx].duration;
    idx++;
    
    if (idx >= count) return false;
    
    if (timing[idx].level || timing[idx].duration < 480 || timing[idx].duration > 720) {
        return false;  // 引导码低电平应该是600μs（容差±20%）
    }
    idx++;
    
    int dataBits = 0;
    // 根据引导码高电平时间判断数据位数
    if (headerHigh >= 1920 && headerHigh <= 2880) {  // 2400 * 0.8 = 1920, 2400 * 1.2 = 2880
        dataBits = 12;
    } else if (headerHigh >= 3840 && headerHigh <= 5760) {  // 4800 * 0.8 = 3840, 4800 * 1.2 = 5760
        // 需要进一步判断是15位还是20位
        // 根据数据位数量判断（15位需要约30个时序点，20位需要约40个时序点）
        int remainingPoints = count - idx;
        if (remainingPoints >= 35) {
            dataBits = 20;
        } else {
            dataBits = 15;
        }
    } else {
        return false;  // 引导码时间不符合Sony协议
    }
    
    // 解析数据位
    uint32_t data = 0;
    
    for (int bit = 0; bit < dataBits; bit++) {
        if (idx >= count - 1) {
            return false;
        }
        
        // 数据位：1200μs高 + 600μs低（1）或600μs高 + 600μs低（0）
        bool highLevel = timing[idx].level;
        unsigned long highTime = 0;
        
        while (idx < count && timing[idx].level == highLevel) {
            highTime += timing[idx].duration;
            idx++;
        }
        
        if (idx >= count) return false;
        
        // 低电平部分（600μs）
        unsigned long lowTime = 0;
        bool lowLevel = timing[idx].level;
        while (idx < count && timing[idx].level == lowLevel) {
            lowTime += timing[idx].duration;
            idx++;
        }
        
        // 判断逻辑值：根据高电平时间
        // 逻辑1: 1200μs高（容差±20% = 960-1440μs）
        // 逻辑0: 600μs高（容差±20% = 480-720μs）
        if (highTime >= 960) {  // 1200 * 0.8 = 960
            // 逻辑1
            data |= (1UL << (dataBits - 1 - bit));  // 从高位开始
        } else if (highTime >= 480) {  // 600 * 0.8 = 480
            // 逻辑0
            // data位保持0
        } else {
            return false;  // 高电平时间异常
        }
    }
    
    signalId.protocol = 4;  // Sony
    signalId.data = data;
    return true;
}

bool IRParser::parseSamsung(const IRTiming* timing, int count, SignalID& signalId) {
    // Samsung协议解析
    // 载波频率：38kHz
    // 引导码：4500μs高 + 4500μs低
    // 数据位：560μs低 + 载波（0: 560μs, 1: 1680μs）
    // 数据格式：32位（地址16位 + 命令16位）
    
    if (count < 68) return false;  // Samsung协议至少需要68个时序点（引导码2个+32位*2=66个）
    
    int idx = 0;
    
    // 检查引导码：第一个应该是高电平，持续时间约4500μs（容差±20%）
    if (!timing[idx].level || timing[idx].duration < 3600 || timing[idx].duration > 5400) {
        return false;
    }
    idx++;
    
    if (idx >= count) return false;
    
    // 引导码的低电平部分，约4500μs（容差±20%）
    if (timing[idx].level || timing[idx].duration < 3600 || timing[idx].duration > 5400) {
        return false;
    }
    idx++;
    
    // 解析32位数据
    uint32_t data = 0;
    for (int bit = 0; bit < 32; bit++) {
        if (idx >= count - 1) {
            return false;  // 数据不足
        }
        
        // 每个位由560μs低电平 + 载波时间组成
        // 逻辑0: 560μs载波（容差±20%）
        // 逻辑1: 1680μs载波（容差±20%）
        if (timing[idx].level) {
            return false;  // 应该是低电平
        }
        
        // 放宽容差范围到±20%
        if (timing[idx].duration < 448 || timing[idx].duration > 672) {
            return false;  // 低电平持续时间应该在560μs左右
        }
        idx++;
        
        if (idx >= count) {
            return false;
        }
        
        // 检查载波时间判断是0还是1
        if (!timing[idx].level) {
            return false;  // 应该是高电平（载波）
        }
        
        unsigned long carrierTime = timing[idx].duration;
        // 放宽容差范围到±20%
        if (carrierTime > 1344) {  // 1680 * 0.8 = 1344
            // 逻辑1：载波时间约1680μs
            data |= (1UL << bit);
        } else if (carrierTime > 448) {  // 560 * 0.8 = 448
            // 逻辑0：载波时间约560μs
            // data位保持0
        } else {
            return false;  // 载波时间异常
        }
        idx++;
    }
    
    // Samsung协议使用32位数据：地址(16位) + 命令(16位)
    signalId.protocol = 5;  // Samsung
    signalId.data = data;  // 使用完整的32位数据
    return true;
}

bool IRParser::isNoise(unsigned long duration) const {
    return duration < 50;  // 小于50μs认为是噪声
}

String IRParser::signalToJSON(const SignalID& signalId) const {
    String json = "{";
    json += "\"protocol\":" + String(signalId.protocol) + ",";
    json += "\"data\":" + String(signalId.data) + ",";
    json += "\"hash\":" + String(signalId.hash) + ",";
    json += "\"isProtocol\":" + String(signalId.isProtocol ? "true" : "false");
    json += "}";
    return json;
}

bool IRParser::signalFromJSON(const String& json, SignalID& signalId) {
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, json);
    
    if (error) {
        return false;
    }
    
    signalId.protocol = doc["protocol"] | 0;
    signalId.data = doc["data"] | 0;
    signalId.hash = doc["hash"] | 0;
    signalId.isProtocol = doc["isProtocol"] | false;
    
    return true;
}

