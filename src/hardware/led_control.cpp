#include "led_control.h"

LEDControl::LEDControl() 
    : currentState(LED_OFF), lastToggle(0), 
      blinkCount(0), blinkRemaining(0), ledOn(false) {
}

void LEDControl::init() {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
}

void LEDControl::update() {
    updateBlink();
}

void LEDControl::setState(LEDState state) {
    currentState = state;
    blinkRemaining = 0;
    
    switch (state) {
        case LED_OFF:
            digitalWrite(LED_PIN, LOW);
            ledOn = false;
            break;
        case LED_ON:
            digitalWrite(LED_PIN, HIGH);
            ledOn = true;
            break;
        case LED_SLOW_BLINK:
            // 慢闪：500ms周期
            break;
        case LED_FAST_BLINK:
            // 快闪：200ms周期
            break;
        case LED_BLINK:
            // 闪烁：100ms周期
            break;
        case LED_LONG_ON:
            digitalWrite(LED_PIN, HIGH);
            ledOn = true;
            break;
        case LED_ERROR_BLINK:
            // 异常快闪：50ms周期
            break;
    }
}

void LEDControl::blink(int count) {
    blinkRemaining = count * 2;  // 每次闪烁需要2次切换
    currentState = LED_BLINK;
}

void LEDControl::setNetworkState(bool connected, bool mqttConnected) {
    if (!connected) {
        setState(LED_SLOW_BLINK);  // AP模式慢闪
    } else if (connected && !mqttConnected) {
        setState(LED_FAST_BLINK);  // WiFi连接但MQTT离线
    } else {
        setState(LED_OFF);  // 连接成功待机时关闭LED，防止光污染
    }
}

void LEDControl::setNetworkStateWithMQTTConfig(bool wifiConnected, bool mqttConnected, bool mqttConfigured) {
    if (!wifiConnected) {
        // AP模式或WiFi连接中：慢闪（500ms）
        setState(LED_SLOW_BLINK);
    } else {
        // WiFi已连接
        if (!mqttConfigured) {
            // MQTT未配置：关闭LED（待机状态）
            setState(LED_OFF);
        } else if (mqttConfigured && !mqttConnected) {
            // MQTT已配置但连接失败：快闪（200ms）提示连接问题
            setState(LED_FAST_BLINK);
        } else {
            // MQTT已配置且已连接：关闭LED（待机状态）
            setState(LED_OFF);
        }
    }
}

void LEDControl::updateBlink() {
    unsigned long now = millis();
    unsigned long interval = 500;  // 默认慢闪
    
    switch (currentState) {
        case LED_SLOW_BLINK:
            interval = 500;
            break;
        case LED_FAST_BLINK:
            interval = 200;
            break;
        case LED_BLINK:
            interval = 100;
            break;
        case LED_ERROR_BLINK:
            interval = 50;
            break;
        default:
            return;  // 不需要闪烁
    }
    
    if (now - lastToggle >= interval) {
        lastToggle = now;
        ledOn = !ledOn;
        digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
        
        if (blinkRemaining > 0) {
            blinkRemaining--;
            if (blinkRemaining == 0) {
                currentState = LED_OFF;
                digitalWrite(LED_PIN, LOW);
            }
        }
    }
}

