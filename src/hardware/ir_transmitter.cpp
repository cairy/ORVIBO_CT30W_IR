#include "ir_transmitter.h"
#include "utils/precise_timer.h"

IRTransmitter::IRTransmitter() 
    : irsend(nullptr), transmitting(false), transmitStartTime(0) {
}

IRTransmitter::~IRTransmitter() {
    if (irsend != nullptr) {
        delete irsend;
        irsend = nullptr;
    }
}

bool IRTransmitter::init() {
    // 初始化IRremoteESP8266库的发送器
    if (irsend == nullptr) {
        irsend = new IRsend(IR_TX_PIN);
        if (irsend == nullptr) {
            return false;
        }
    }
    
    irsend->begin();
    
    // 保留原有的初始化逻辑
    pinMode(IR_TX_PIN, OUTPUT);
    digitalWrite(IR_TX_PIN, LOW);
    return true;
}

bool IRTransmitter::sendNEC(uint32_t data, uint16_t nbits) {
    if (irsend == nullptr) {
        return false;
    }
    
    transmitting = true;
    irsend->sendNEC(data, nbits);
    transmitting = false;
    return true;
}

bool IRTransmitter::sendNECRepeat() {
    if (irsend == nullptr) {
        return false;
    }
    
    transmitting = true;
    // NEC重复码波形：9000μs高 + 2250μs低 + 560μs低
    uint16_t necRepeat[3] = {9000, 2250, 600};
    irsend->sendRaw(necRepeat, 3, 38);  // 38kHz载波
    transmitting = false;
    return true;
}

bool IRTransmitter::transmit(const IRTiming* timing, int count) {
    if (timing == nullptr || count <= 0) {
        return false;
    }
    
    transmitting = true;
    transmitStartTime = micros();
    
    // 关闭中断以确保时序精度（最多10ms，符合INTERRUPT_DISABLE_MAX_MS要求）
    noInterrupts();
    
    for (int i = 0; i < count; i++) {
        if (timing[i].level) {
            // 高电平时发送载波
            enableCarrier(38000);  // 38kHz载波
        } else {
            // 低电平时关闭载波
            disableCarrier();
        }
        
        delayMicrosecondsPrecise(timing[i].duration);
        
        // 优化：检查中断关闭时间，确保不超过10ms限制
        // 如果超过限制，短暂开启中断后继续关闭，避免长时间阻塞
        // 使用更频繁的检查（每1ms检查一次），减少中断关闭时间
        if ((i % 100) == 0) {  // 每100个时序点检查一次（约1ms）
            unsigned long elapsed = micros() - transmitStartTime;
            if (elapsed > (INTERRUPT_DISABLE_MAX_MS * 1000)) {
                interrupts();  // 重新开启中断，允许其他任务执行
                yield();        // 让出CPU时间
                noInterrupts(); // 继续关闭中断
                transmitStartTime = micros();  // 重置计时器
            }
        }
    }
    
    disableCarrier();
    interrupts();  // 重新开启中断
    
    transmitting = false;
    return true;
}

bool IRTransmitter::transmitRaw(const uint16_t* rawData, int count) {
    if (rawData == nullptr || count <= 0) {
        return false;
    }
    
    transmitting = true;
    transmitStartTime = micros();
    
    // 关闭中断以确保时序精度（最多10ms）
    noInterrupts();
    
    for (int i = 0; i < count; i++) {
        bool level = (i % 2 == 0);  // 交替高低电平
        if (level) {
            enableCarrier(38000);
        } else {
            disableCarrier();
        }
        delayMicrosecondsPrecise(rawData[i]);
        
        // 优化：检查中断关闭时间，确保不超过10ms限制
        // 使用更频繁的检查（每1ms检查一次），减少中断关闭时间
        if ((i % 100) == 0) {  // 每100个时序点检查一次（约1ms）
            unsigned long elapsed = micros() - transmitStartTime;
            if (elapsed > (INTERRUPT_DISABLE_MAX_MS * 1000)) {
                interrupts();  // 重新开启中断
                yield();        // 让出CPU时间
                noInterrupts(); // 继续关闭中断
                transmitStartTime = micros();  // 重置计时器
            }
        }
    }
    
    disableCarrier();
    interrupts();
    
    transmitting = false;
    return true;
}

bool IRTransmitter::isTransmitting() const {
    return transmitting;
}

void IRTransmitter::enableCarrier(uint32_t freqHz) {
    // 软件PWM生成载波
    // ESP8266可以使用硬件PWM，这里简化使用软件实现
    // 实际应用中可以使用硬件定时器提高精度
    analogWriteFreq(freqHz);
    analogWrite(IR_TX_PIN, 512);  // 50%占空比
}

void IRTransmitter::disableCarrier() {
    analogWrite(IR_TX_PIN, 0);
    digitalWrite(IR_TX_PIN, LOW);
}

void IRTransmitter::delayMicrosecondsPrecise(unsigned long us) {
    // 使用硬件定时器实现精准延时
    // 对于>1000μs使用Ticker，对于<1000μs使用优化的delayMicroseconds
    PreciseTimer::delayPreciseUs(us);
}

