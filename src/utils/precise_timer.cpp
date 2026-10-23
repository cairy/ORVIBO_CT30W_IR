#include "precise_timer.h"
#include "config/constants.h"

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
    if (!timer.startDelay(ms)) {
        // 定时器起不来（Ticker/os_timer 资源耗尽等）⇒ 退化为普通延时，
        // 至少它是有界的，不会像下面的忙等那样可能永远出不来。
        delay(ms);
        return;
    }

    // ⚠️ 等待必须有超时。Ticker 的回调依赖中断投递：若在 noInterrupts() 区间
    // 内调用、os_timer 资源耗尽、或 once_ms 因堆不足失败，delayComplete 永远
    // 不会变 true ⇒ 永久挂起。而 yield() 和 delay() 一样会喂住软看门狗 ⇒
    // **既不崩也不重启** ⇒ SafeMode 的"启动失败计数"永远不涨 ⇒ 空中无救。
    // 这正是项目明令禁止的「喂着狗死等」。
    const unsigned long deadline = millis() + ms + PRECISE_DELAY_GRACE_MS;
    while (!timer.isDelayComplete()) {
        if ((long)(millis() - deadline) >= 0) {   // 这样写对 millis() 回绕是安全的
            timer.cancelDelay();
            Serial.printf_P(PSTR("[PreciseTimer] 延时 %lu ms 超时，强制退出\n"), ms);
            return;
        }
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

