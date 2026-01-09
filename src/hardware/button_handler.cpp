#include "button_handler.h"

ButtonHandler::ButtonHandler()
    : currentState(BUTTON_RELEASED), lastButtonState(HIGH),
      pressStartTime(0), longPressTriggered(false),
      longPressCallback(nullptr), lastCheckTime(0) {
}

bool ButtonHandler::init() {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    lastButtonState = digitalRead(BUTTON_PIN);
    return true;
}

void ButtonHandler::update() {
    unsigned long now = millis();
    
    // 限制检测频率
    if (now - lastCheckTime < CHECK_INTERVAL_MS) {
        return;
    }
    lastCheckTime = now;
    
    // 读取当前按钮状态（INPUT_PULLUP模式下，按下为LOW）
    bool currentButtonState = digitalRead(BUTTON_PIN);
    
    // 防抖处理
    if (currentButtonState == lastButtonState) {
        // 状态稳定，处理逻辑
        if (currentButtonState == LOW) {
            // 按钮被按下
            if (currentState == BUTTON_RELEASED) {
                // 刚按下，记录时间
                pressStartTime = now;
                currentState = BUTTON_PRESSED;
                longPressTriggered = false;
            } else if (currentState == BUTTON_PRESSED) {
                // 持续按下，检查是否达到长按时间
                unsigned long pressDuration = now - pressStartTime;
                if (pressDuration >= LONG_PRESS_TIME_MS && !longPressTriggered) {
                    // 触发长按
                    currentState = BUTTON_LONG_PRESSED;
                    longPressTriggered = true;
                    
                    // 调用回调函数
                    if (longPressCallback != nullptr) {
                        longPressCallback();
                    }
                }
            }
        } else {
            // 按钮释放
            if (currentState != BUTTON_RELEASED) {
                currentState = BUTTON_RELEASED;
                longPressTriggered = false;
                pressStartTime = 0;
            }
        }
    }
    
    lastButtonState = currentButtonState;
}
