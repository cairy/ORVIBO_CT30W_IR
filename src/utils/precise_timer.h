#ifndef PRECISE_TIMER_H
#define PRECISE_TIMER_H

#include <Arduino.h>
#include <Ticker.h>

// 精准定时器工具类
// 使用ESP8266 Ticker库实现硬件定时器（毫秒级），delayMicroseconds（微秒级）
// 注意：Ticker在noInterrupts()状态下无法工作，delayPreciseUs统一使用delayMicroseconds
// 误差要求：≤ 20ms（毫秒级延时）
class PreciseTimer {
public:
    PreciseTimer();
    ~PreciseTimer();
    
    // 阻塞式精准延时（毫秒）
    // 使用Ticker硬件定时器（需要中断开启状态），误差 ≤ 20ms
    // 注意：在noInterrupts()状态下调用时，Ticker无法工作
    static void delayPreciseMs(unsigned long ms);
    
    // 非阻塞式延时开始（毫秒）
    // 返回true表示延时已开始，false表示上一个延时未完成
    // 注意：需要中断开启状态才能正常工作
    bool startDelay(unsigned long ms);
    
    // 检查非阻塞延时是否完成
    bool isDelayComplete() const;
    
    // 取消非阻塞延时
    void cancelDelay();
    
    // 微秒级精准延时
    // 统一使用delayMicroseconds，适用于中断关闭状态
    // ESP8266的delayMicroseconds精度足够，适用于微秒级延时
    static void delayPreciseUs(unsigned long us);

private:
    Ticker ticker;
    volatile bool delayComplete;
    void onTimer();
};

#endif // PRECISE_TIMER_H

