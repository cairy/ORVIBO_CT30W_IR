#include "device_state.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

bool DeviceState::forwardingEnabled = true;
bool DeviceState::loaded = false;

// 与 CONFIG_FILE_* 一起放在 constants.h 的风格里，但只有这一处用得上，就近定义
static const char* DEVICE_STATE_FILE = "/device_state.json";

bool DeviceState::init() {
    forwardingEnabled = true;  // 默认开：没配过就等于老行为
    loaded = false;

    if (!LittleFS.exists(DEVICE_STATE_FILE)) {
        Serial.println(F("[设备状态] 无状态文件，转发默认开启"));
        loaded = true;
        return true;
    }

    File file = LittleFS.open(DEVICE_STATE_FILE, "r");
    if (!file) {
        Serial.println(F("[设备状态] 状态文件打开失败，转发默认开启"));
        loaded = true;
        return false;
    }

    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
        Serial.printf_P(PSTR("[设备状态] 状态文件解析失败(%s)，转发默认开启\n"), err.c_str());
        loaded = true;
        return false;
    }

    // ⚠️ 这里的键名**故意**与 API 字段的 camelCase 不一致（API 是 forwardingEnabled）。
    // 它是设备端私有配置文件的格式，不是对外契约 —— 改名等于升级后读不出用户原有设置
    // （会静默回落到"转发默认开启"）。私有格式只讲兼容，不讲命名统一。
    if (doc.containsKey("forwarding_enabled")) {
        forwardingEnabled = doc["forwarding_enabled"].as<bool>();
    }
    loaded = true;

    Serial.printf_P(PSTR("[设备状态] 已加载：转发=%s\n"), forwardingEnabled ? "开" : "关");
    return true;
}

bool DeviceState::isForwardingEnabled() {
    return forwardingEnabled;
}

bool DeviceState::setForwardingEnabled(bool enabled) {
    if (loaded && forwardingEnabled == enabled) {
        return true;  // 没变化就不写 flash（省擦写寿命）
    }
    forwardingEnabled = enabled;
    return save();
}

bool DeviceState::save() {
    StaticJsonDocument<128> doc;
    doc["forwarding_enabled"] = forwardingEnabled;

    File file = LittleFS.open(DEVICE_STATE_FILE, "w");
    if (!file) {
        Serial.println(F("[设备状态] 状态文件写入失败"));
        return false;
    }
    serializeJson(doc, file);
    file.close();

    Serial.printf_P(PSTR("[设备状态] 已保存：转发=%s\n"), forwardingEnabled ? "开" : "关");
    return true;
}
