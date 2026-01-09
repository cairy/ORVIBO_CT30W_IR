#include "message_queue.h"

MessageQueue::MessageQueue() : head(0), tail(0), count(0) {
}

bool MessageQueue::push(const ForwardTask& task) {
    noInterrupts();
    if (count >= QUEUE_SIZE) {
        interrupts();
        return false;  // 队列已满
    }
    
    queue[tail] = task;
    tail = (tail + 1) % QUEUE_SIZE;
    count++;
    
    interrupts();
    return true;
}

bool MessageQueue::pop(ForwardTask& task) {
    noInterrupts();
    if (count == 0) {
        interrupts();
        return false;  // 队列为空
    }
    
    task = queue[head];
    head = (head + 1) % QUEUE_SIZE;
    count--;
    
    interrupts();
    return true;
}

bool MessageQueue::isEmpty() const {
    return count == 0;
}

bool MessageQueue::isFull() const {
    return count >= QUEUE_SIZE;
}

int MessageQueue::size() const {
    return count;
}

void MessageQueue::clear() {
    noInterrupts();
    head = 0;
    tail = 0;
    count = 0;
    interrupts();
}

