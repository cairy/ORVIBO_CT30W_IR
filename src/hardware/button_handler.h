#ifndef BUTTON_HANDLER_H
#define BUTTON_HANDLER_H

#include <Arduino.h>
#include "config/pins.h"

// 按钮状态
enum ButtonState {
    BUTTON_RELEASED,    // 释放
    BUTTON_PRESSED,     // 按下
    BUTTON_LONG_PRESSED // 长按
};

// 按钮处理器
class ButtonHandler {
public:
    ButtonHandler();
    bool init();
    void update();
    
    // 设置长按回调函数
    void setLongPressCallback(void (*callback)()) {
        longPressCallback = callback;
    }
    
    // 获取当前按钮状态
    ButtonState getState() const { return currentState; }
    
private:
    ButtonState currentState;
    bool lastButtonState;
    unsigned long pressStartTime;
    bool longPressTriggered;
    
    // 回调函数
    void (*longPressCallback)();
    
    // 常量
    static const unsigned long DEBOUNCE_TIME_MS = 50;      // 防抖时间
    static const unsigned long LONG_PRESS_TIME_MS = 5000;   // 长按时间（5秒）
    static const unsigned long CHECK_INTERVAL_MS = 20;      // 检测间隔（20ms）
    
    unsigned long lastCheckTime;
};

#endif // BUTTON_HANDLER_H
