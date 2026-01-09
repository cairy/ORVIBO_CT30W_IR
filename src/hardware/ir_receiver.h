#ifndef IR_RECEIVER_H
#define IR_RECEIVER_H

#include <Arduino.h>
#include "config/pins.h"
#include "config/constants.h"

// 红外时序数据点
struct IRTiming {
    bool level;        // 电平（true=HIGH, false=LOW）
    unsigned long duration;  // 持续时间（微秒）
};

// 红外接收器
class IRReceiver {
public:
    IRReceiver();
    bool init();
    void update();
    bool getSignal(IRTiming* buffer, int& count, int maxCount);
    void clearBuffer();
    bool isReceiving() const;
    
    // 中断回调函数（静态）
    static void IRAM_ATTR onInterrupt();

private:
    static IRReceiver* instance;
    
    // 环形缓冲区
    struct BufferEntry {
        bool level;
        unsigned long timestamp;
        bool valid;
    };
    
    BufferEntry buffer[IR_RX_BUFFER_SIZE / sizeof(BufferEntry)];  // 静态分配缓冲区
    volatile int writeIndex;
    volatile int readIndex;
    volatile int bufferCount;
    volatile bool receiving;
    unsigned long lastSignalTime;
    
    static const int BUFFER_SIZE = IR_RX_BUFFER_SIZE / sizeof(BufferEntry);
    
    void processBuffer();
    int getAvailableCount() const;
};

#endif // IR_RECEIVER_H

