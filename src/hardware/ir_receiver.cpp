#include "ir_receiver.h"

IRReceiver* IRReceiver::instance = nullptr;

IRReceiver::IRReceiver() 
    : writeIndex(0), readIndex(0), bufferCount(0), 
      receiving(false), lastSignalTime(0) {
    instance = this;
    // 静态缓冲区已自动初始化，只需设置valid标志
    for (int i = 0; i < BUFFER_SIZE; i++) {
        buffer[i].valid = false;
    }
}

bool IRReceiver::init() {
    pinMode(IR_RX_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(IR_RX_PIN), onInterrupt, CHANGE);
    return true;
}

void IRReceiver::update() {
    unsigned long now = micros();
    
    // 检查是否超时（500ms无信号则认为接收完成）
    if (receiving && (now - lastSignalTime) > (SIGNAL_TIMEOUT_MS * 1000)) {
        receiving = false;
    }
    
    processBuffer();
}

void IRAM_ATTR IRReceiver::onInterrupt() {
    if (instance == nullptr) return;
    
    IRReceiver* receiver = instance;
    unsigned long now = micros();
    bool level = digitalRead(IR_RX_PIN) == HIGH;
    
    // 覆盖机制：如果缓冲区满，覆盖最旧的数据
    if (receiver->bufferCount >= BUFFER_SIZE) {
        receiver->readIndex = (receiver->readIndex + 1) % BUFFER_SIZE;
        receiver->bufferCount--;
    }
    
    receiver->buffer[receiver->writeIndex].level = level;
    receiver->buffer[receiver->writeIndex].timestamp = now;
    receiver->buffer[receiver->writeIndex].valid = true;
    
    receiver->writeIndex = (receiver->writeIndex + 1) % BUFFER_SIZE;
    receiver->bufferCount++;
    receiver->receiving = true;
    receiver->lastSignalTime = now;
}

bool IRReceiver::getSignal(IRTiming* outputBuffer, int& count, int maxCount) {
    if (outputBuffer == nullptr || maxCount <= 0) {
        return false;
    }
    
    if (bufferCount == 0 || receiving) {
        count = 0;
        return false;
    }
    
    // 计算持续时间
    count = 0;
    int startIdx = readIndex;
    unsigned long lastTime = buffer[startIdx].timestamp;
    
    for (int i = 0; i < bufferCount && count < maxCount; i++) {
        int idx = (startIdx + i) % BUFFER_SIZE;
        if (!buffer[idx].valid) break;
        
        unsigned long currentTime = buffer[idx].timestamp;
        unsigned long duration = currentTime - lastTime;
        
        if (i > 0) {  // 第一个点不需要计算持续时间
            outputBuffer[count - 1].duration = duration;
        }
        
        if (count < maxCount) {
            outputBuffer[count].level = buffer[idx].level;
            outputBuffer[count].duration = 0;  // 将在下一次迭代中设置
            count++;
        }
        
        lastTime = currentTime;
    }
    
    // 清空缓冲区
    clearBuffer();
    
    return (count > 0);
}

void IRReceiver::clearBuffer() {
    readIndex = writeIndex;
    bufferCount = 0;
    receiving = false;
    for (int i = 0; i < BUFFER_SIZE; i++) {
        buffer[i].valid = false;
    }
}

bool IRReceiver::isReceiving() const {
    return receiving;
}

void IRReceiver::processBuffer() {
    // 在主循环中处理缓冲区数据
    // 这里可以添加额外的处理逻辑
}

int IRReceiver::getAvailableCount() const {
    return bufferCount;
}

