#ifndef MESSAGE_QUEUE_H
#define MESSAGE_QUEUE_H

#include <Arduino.h>

// 转发任务消息结构
struct ForwardTask {
    uint32_t signal_id;              // 信号标识
    uint8_t signal_count;             // B信号数量
    uint32_t signal_data[5];          // B信号数据（最多5个）
    uint16_t intervals[4];            // 信号间隔（4个间隔对应5个信号）
    uint8_t repeat_count;             // 重复次数
    bool valid;                        // 消息是否有效
};

// 线程安全消息队列（使用中断禁用保护临界区）
class MessageQueue {
public:
    MessageQueue();
    bool push(const ForwardTask& task);
    bool pop(ForwardTask& task);
    bool isEmpty() const;
    bool isFull() const;
    int size() const;
    void clear();

private:
    static const int QUEUE_SIZE = 3;  // 固定大小数组，3个信号缓存（从5减少到3，节省约70字节）
    ForwardTask queue[QUEUE_SIZE];
    volatile int head;
    volatile int tail;
    volatile int count;
};

#endif // MESSAGE_QUEUE_H

