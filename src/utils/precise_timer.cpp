#include "precise_timer.h"

PreciseTimer::PreciseTimer() : delayComplete(true) {
}

PreciseTimer::~PreciseTimer() {
    cancelDelay();
}

void PreciseTimer::delayPreciseMs(unsigned long ms) {
    if (ms == 0) return;
    
    // 对于较短的延时（<10ms），使用delayMicroseconds提高精度
    if (ms < 10) {
        delayMicroseconds(ms * 1000);
        return;
    }
    
    // 对于≥10ms的延时，使用Ticker（需要中断开启状态）
    // 注意：如果在noInterrupts()状态下调用，Ticker无法工作
    // 红外时序发送场景下，延时都是微秒级，不会调用此函数
    PreciseTimer timer;
    timer.startDelay(ms);
    
    // 等待延时完成
    while (!timer.isDelayComplete()) {
        yield();  // 让出CPU时间，避免阻塞其他任务
    }
}

bool PreciseTimer::startDelay(unsigned long ms) {
    if (!delayComplete) {
        return false;  // 上一个延时未完成
    }
    
    delayComplete = false;
    
    // 使用Ticker在指定时间后触发回调
    // ESP8266 Ticker库支持once_ms方法，使用lambda捕获this指针
    ticker.once_ms(ms, [this]() {
        this->onTimer();
    });
    
    return true;
}

bool PreciseTimer::isDelayComplete() const {
    return delayComplete;
}

void PreciseTimer::cancelDelay() {
    ticker.detach();
    delayComplete = true;
}

void PreciseTimer::onTimer() {
    delayComplete = true;
}

void PreciseTimer::delayPreciseUs(unsigned long us) {
    if (us == 0) return;
    
    // 在中断关闭状态下，Ticker无法工作，统一使用delayMicroseconds
    // ESP8266的delayMicroseconds精度足够，适用于微秒级延时
    // 对于长时间延时（>10ms），在中断关闭状态下不推荐，但为了可靠性也使用delayMicroseconds
    delayMicroseconds(us);
}

