#ifndef DEVICE_STATE_H
#define DEVICE_STATE_H

#include <Arduino.h>
#include "config/constants.h"

// 设备级运行状态（存 /device_state.json）
//
// 与 NetworkConfig 分开存：那边是"怎么连网"，这边是"设备怎么干活"，职责不同。
//
// 目前只有一项：转发总开关。
// 落盘的原因：之前它只活在内存里（IRForwarder::enabled，构造时恒为 true），
// 一重启就变回"开"。用户明明关过，回头发现它自己开了，只会怀疑设备坏了。
class DeviceState {
public:
    // 从文件系统加载。文件不存在时保留默认值（转发开），不算错误。
    static bool init();

    static bool isForwardingEnabled();
    // 改开关并立刻落盘；返回落盘是否成功
    static bool setForwardingEnabled(bool enabled);

private:
    static bool forwardingEnabled;
    static bool loaded;
    static bool save();
};

#endif // DEVICE_STATE_H
