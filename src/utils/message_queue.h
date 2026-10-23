#ifndef MESSAGE_QUEUE_H
#define MESSAGE_QUEUE_H

#include <Arduino.h>
#include "config/constants.h"

// 转发任务最多能带几个 B 信号。
// ⚠️ 必须与 MAX_B_SIGNALS_PER_RULE（规则允许的上限，当前 6）保持一致 ——
// 以前这里是 3，于是配了 4~6 个 B 信号的规则在异步转发链路（delay / queue）上
// 会被静默截掉，而"试跑"（testForward 走同步 executeForward）会发全部 6 个
// ⇒ 表现就是「试跑成功、实际少发一半」。
#define FORWARD_TASK_MAX_B MAX_B_SIGNALS_PER_RULE

// 转发任务消息结构
struct ForwardTask {
    uint8_t signal_count;             // B信号数量
    uint32_t signal_data[2 + FORWARD_TASK_MAX_B];  // [0]=A协议, [1]=A数据, [2+]=B数据
    uint8_t b_protocols[FORWARD_TASK_MAX_B];       // B信号协议
    uint16_t intervals[FORWARD_TASK_MAX_B];        // 间隔（含"第一个信号前的间隔"时最多 signalCount 个）
    uint8_t repeat_count;             // 重复次数（取自 B 序列首个信号的 repeatCount）
    uint8_t rule_id;                  // 规则ID（用于调试）
    // 这条任务对应的转发记录（下标 + 身份号）。任务是异步执行的（延时槽 / 本队列），
    // 执行时"最新一条记录"可能已经是别人的了，所以必须把归属带在任务里。
    // 身份号对不上（记录已被后来的覆盖）就放弃回填，宁缺勿错。
    uint8_t record_index;
    uint16_t record_seq;
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

