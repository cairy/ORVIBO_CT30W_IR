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
    // 注意：这里【不】挂中断。GPIO5 在正常工作模式下归 IRrecv（irParser）所有，
    // 只有进入学习模式才切到本接收器 —— 见 IRReceiver::attach()。
    // 以前这里直接 attachInterrupt()，随后 irParser.init() 里的
    // irrecv->enableIRIn() 会把它无声覆盖（core 的 attachInterrupt 是单槽覆盖，
    // 不是链表）⇒ 本类全程是死代码，而 main.cpp 里那两行的先后顺序成了
    // 无人知晓的隐式依赖。现在改成显式切换，顺序不再影响结果。
    return true;
}

void IRReceiver::attach() {
    attachInterrupt(digitalPinToInterrupt(IR_RX_PIN), onInterrupt, CHANGE);
}

void IRReceiver::detach() {
    detachInterrupt(digitalPinToInterrupt(IR_RX_PIN));
}

void IRReceiver::update() {
    unsigned long now = micros();
    
    // 检查是否超时（500ms无信号则认为接收完成）
    if (receiving) {
        // 无符号减法本身就正确处理回绕（micros() 约 71 分钟回绕一次）。
        // 原来那个 "now < lastSignalTime ⇒ 0xFFFFFFFF - last + now" 的分支不但多余，
        // 算出来还比正确值**小 1 微秒**（回绕点的算术差）。
        unsigned long elapsed = now - lastSignalTime;
        if (elapsed > (SIGNAL_TIMEOUT_MS * 1000)) {
            receiving = false;
        }
    }
    
    processBuffer();
}

void IRAM_ATTR IRReceiver::onInterrupt() {
    if (instance == nullptr) return;
    
    IRReceiver* receiver = instance;
    unsigned long now = micros();
    bool level = digitalRead(IR_RX_PIN) == HIGH;
    
    // 优化：确保关键操作的原子性
    // 使用局部变量减少对volatile变量的访问次数
    int currentWriteIndex = receiver->writeIndex;
    int currentBufferCount = receiver->bufferCount;
    
    // 覆盖机制：如果缓冲区满，覆盖最旧的数据
    if (currentBufferCount >= BUFFER_SIZE) {
        receiver->readIndex = (receiver->readIndex + 1) % BUFFER_SIZE;
        currentBufferCount = BUFFER_SIZE - 1;  // 保持bufferCount不变
    }
    
    // 写入缓冲区（原子操作：单个结构体赋值）
    receiver->buffer[currentWriteIndex].level = level;
    receiver->buffer[currentWriteIndex].timestamp = now;
    receiver->buffer[currentWriteIndex].valid = true;
    
    // 更新索引和计数（原子操作）
    currentWriteIndex = (currentWriteIndex + 1) % BUFFER_SIZE;
    receiver->writeIndex = currentWriteIndex;
    receiver->bufferCount = currentBufferCount + 1;
    receiver->receiving = true;
    receiver->lastSignalTime = now;
}

bool IRReceiver::getSignal(IRTiming* outputBuffer, int& count, int maxCount) {
    if (outputBuffer == nullptr || maxCount <= 0) {
        return false;
    }

    count = 0;

    // ⚠️ 整段读取都在**同一个关中断窗口**里完成。
    // 以前是"关中断只抄几个下标 → 开中断 → 再逐个读 buffer[...]"：ISR 同时在写
    // 同一个环形缓冲（缓冲满时还会推进 readIndex），于是可能读到"半写"的条目
    // （level/timestamp 已是新值、valid 还是旧值，或 startIdx 指向的槽位刚被 ISR
    // 挪走）。后果不是崩溃，而是**学到一条不可用的时序码**，现象只是"这个键怎么
    // 学都学不出来"，极难定位。
    // 关中断窗口的长度 = 条目数 × 几条指令，上限 BUFFER_SIZE 条，约几十微秒；
    // 与本项目其它关中断窗口（发一条红外最长 67ms）相比可以忽略。
    noInterrupts();

    int snapCount = bufferCount;
    if (snapCount == 0) {
        interrupts();
        return false;
    }

    if (receiving) {
        unsigned long now = micros();
        // 无符号减法本身就正确处理了回绕（micros() 约 71 分钟回绕一次）。
        // 手写的 `0xFFFFFFFF - last + now` 分支不仅多余，算出来还**小 1 微秒**。
        unsigned long elapsed = now - lastSignalTime;
        if (elapsed < 100000) {
            // 信号还在进来，等它收完再取
            interrupts();
            return false;
        }
        receiving = false;
    }

    int startIdx = readIndex;
    unsigned long lastTime = buffer[startIdx].timestamp;

    for (int i = 0; i < snapCount && count < maxCount; i++) {
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

    // 取完就清空。放在同一个关中断窗口里，省一次开/关中断
    readIndex = writeIndex;
    bufferCount = 0;
    receiving = false;

    interrupts();

    return (count > 0);
}

void IRReceiver::clearBuffer() {
    noInterrupts();
    readIndex = writeIndex;
    bufferCount = 0;
    receiving = false;
    interrupts();

    // 不需要清空整个缓冲区，只需重置索引和计数
    // buffer[i].valid 会在下次写入时被覆盖
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

