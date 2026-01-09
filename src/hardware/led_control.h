#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include <Arduino.h>
#include "config/pins.h"

// LED 状态类型
enum LEDState {
    LED_OFF,
    LED_ON,
    LED_SLOW_BLINK,    // 慢闪（AP模式）
    LED_FAST_BLINK,    // 快闪（WiFi连接中）
    LED_BLINK,         // 闪烁（接收信号）
    LED_LONG_ON,       // 长亮（发射信号）
    LED_ERROR_BLINK    // 异常快闪
};

// LED 控制器
class LEDControl {
public:
    LEDControl();
    void init();
    void update();
    void setState(LEDState state);
    void blink(int count = 1);
    void setNetworkState(bool connected, bool mqttConnected);
    void setNetworkStateWithMQTTConfig(bool wifiConnected, bool mqttConnected, bool mqttConfigured);

private:
    LEDState currentState;
    unsigned long lastToggle;
    int blinkCount;
    int blinkRemaining;
    bool ledOn;
    
    void updateBlink();
};

#endif // LED_CONTROL_H

