#include "network_config.h"
#include "config/constants.h"
#include "utils/storage.h"
#include <ArduinoJson.h>

const char* NetworkConfigManager::CONFIG_FILE = CONFIG_FILE_NETWORK;
const char* NetworkConfigManager::BACKUP_FILE = CONFIG_FILE_NETWORK_BAK;

NetworkConfigManager::NetworkConfigManager() {
    config.valid = false;
    memset(config.wifi_ssid, 0, sizeof(config.wifi_ssid));
    memset(config.wifi_password, 0, sizeof(config.wifi_password));
    memset(config.mqtt_server, 0, sizeof(config.mqtt_server));
    config.mqtt_port = 1883;
    memset(config.mqtt_username, 0, sizeof(config.mqtt_username));
    memset(config.mqtt_password, 0, sizeof(config.mqtt_password));
}

bool NetworkConfigManager::load() {
    return loadFromFlash();
}

bool NetworkConfigManager::save() {
    return saveToFlash();
}

NetworkConfig& NetworkConfigManager::getConfig() {
    return config;
}

void NetworkConfigManager::setWiFi(const String& ssid, const String& password) {
    strncpy(config.wifi_ssid, ssid.c_str(), sizeof(config.wifi_ssid) - 1);
    strncpy(config.wifi_password, password.c_str(), sizeof(config.wifi_password) - 1);
    config.wifi_ssid[sizeof(config.wifi_ssid) - 1] = '\0';
    config.wifi_password[sizeof(config.wifi_password) - 1] = '\0';
}

void NetworkConfigManager::setMQTT(const String& server, uint16_t port,
                                   const String& username, const String& password) {
    strncpy(config.mqtt_server, server.c_str(), sizeof(config.mqtt_server) - 1);
    config.mqtt_port = port;
    strncpy(config.mqtt_username, username.c_str(), sizeof(config.mqtt_username) - 1);
    strncpy(config.mqtt_password, password.c_str(), sizeof(config.mqtt_password) - 1);
    config.mqtt_server[sizeof(config.mqtt_server) - 1] = '\0';
    config.mqtt_username[sizeof(config.mqtt_username) - 1] = '\0';
    config.mqtt_password[sizeof(config.mqtt_password) - 1] = '\0';
}

// ── 入库前长度校验 ────────────────────────────────────────────────────────
// 尺寸全部取自上面 struct 里的字段声明，改字段就必须同步改这里（同一份事实只有一个出处）。
String NetworkConfigManager::checkWiFi(const String& ssid, const String& password,
                                       bool enforceMinLength) {
    if (ssid.length() == 0) return F("WiFi 名称不能为空");
    if (ssid.length() > sizeof(NetworkConfig::wifi_ssid) - 1) {
        return String(F("WiFi 名称最长 ")) + String(sizeof(NetworkConfig::wifi_ssid) - 1) +
               F(" 字节，当前 ") + String(ssid.length()) + F(" 字节（超长会被悄悄截断）");
    }
    // 密码留空 = 开放网络，允许
    if (password.length() == 0) return String();
    if (password.length() > sizeof(NetworkConfig::wifi_password) - 1) {
        return String(F("WiFi 密码最长 ")) + String(sizeof(NetworkConfig::wifi_password) - 1) +
               F(" 个字符，当前 ") + String(password.length()) +
               F(" 个（超长会被悄悄截断，导致连不上并退回热点模式）");
    }
    if (enforceMinLength && password.length() < 8) {
        return String(F("WPA/WPA2 密码至少 8 个字符，当前 ")) + String(password.length()) + F(" 个");
    }
    return String();
}

String NetworkConfigManager::checkMQTT(const String& server, uint16_t port,
                                       const String& username, const String& password) {
    if (server.length() > sizeof(NetworkConfig::mqtt_server) - 1) {
        return String(F("MQTT 服务器名最长 ")) + String(sizeof(NetworkConfig::mqtt_server) - 1) +
               F(" 字节，当前 ") + String(server.length()) + F(" 字节");
    }
    // 端口 0 是"toInt() 失败"的返回值，绝不能原样写进去（否则 MQTT 永远连不上，
    // 而界面上看不出任何异常）。网页表单有"留空=沿用原值"的规则先兜一层，
    // 这里再兜一层：任何入口最后都要过这道闸。
    if (port == 0) {
        return F("MQTT 端口无效（应为 1~65535）");
    }
    if (username.length() > sizeof(NetworkConfig::mqtt_username) - 1) {
        return String(F("MQTT 用户名最长 ")) + String(sizeof(NetworkConfig::mqtt_username) - 1) +
               F(" 字节，当前 ") + String(username.length()) + F(" 字节");
    }
    if (password.length() > sizeof(NetworkConfig::mqtt_password) - 1) {
        return String(F("MQTT 密码最长 ")) + String(sizeof(NetworkConfig::mqtt_password) - 1) +
               F(" 字节，当前 ") + String(password.length()) + F(" 字节");
    }
    return String();
}

void NetworkConfigManager::clearMQTT() {
    memset(config.mqtt_server, 0, sizeof(config.mqtt_server));
    config.mqtt_port = 1883;
    memset(config.mqtt_username, 0, sizeof(config.mqtt_username));
    memset(config.mqtt_password, 0, sizeof(config.mqtt_password));
}

bool NetworkConfigManager::isValid() const {
    return config.valid && strlen(config.wifi_ssid) > 0;
}

bool NetworkConfigManager::isMQTTConfigured() const {
    return strlen(config.mqtt_server) > 0;
}

void NetworkConfigManager::reset() {
    config.valid = false;
    memset(config.wifi_ssid, 0, sizeof(config.wifi_ssid));
    memset(config.wifi_password, 0, sizeof(config.wifi_password));
    memset(config.mqtt_server, 0, sizeof(config.mqtt_server));
    config.mqtt_port = 1883;
    memset(config.mqtt_username, 0, sizeof(config.mqtt_username));
    memset(config.mqtt_password, 0, sizeof(config.mqtt_password));
    saveToFlash();
}

bool NetworkConfigManager::loadFromFile(const char* path) {
    String content = Storage::readFile(path);
    if (content.length() == 0) {
        return false;
    }

    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, content);
    if (error) {
        return false;
    }

    strncpy(config.wifi_ssid, doc["wifi_ssid"] | "", sizeof(config.wifi_ssid) - 1);
    strncpy(config.wifi_password, doc["wifi_password"] | "", sizeof(config.wifi_password) - 1);
    strncpy(config.mqtt_server, doc["mqtt_server"] | "", sizeof(config.mqtt_server) - 1);
    config.mqtt_port = doc["mqtt_port"] | 1883;
    strncpy(config.mqtt_username, doc["mqtt_username"] | "", sizeof(config.mqtt_username) - 1);
    strncpy(config.mqtt_password, doc["mqtt_password"] | "", sizeof(config.mqtt_password) - 1);

    // strncpy 不会补尾零（长度刚好时会截断但不保证 '\0'），这里统一兜底
    config.wifi_ssid[sizeof(config.wifi_ssid) - 1] = '\0';
    config.wifi_password[sizeof(config.wifi_password) - 1] = '\0';
    config.mqtt_server[sizeof(config.mqtt_server) - 1] = '\0';
    config.mqtt_username[sizeof(config.mqtt_username) - 1] = '\0';
    config.mqtt_password[sizeof(config.mqtt_password) - 1] = '\0';

    config.valid = (strlen(config.wifi_ssid) > 0);
    return config.valid;
}

bool NetworkConfigManager::writeConfigTo(const char* path) {
    DynamicJsonDocument doc(1024);
    doc["wifi_ssid"] = config.wifi_ssid;
    doc["wifi_password"] = config.wifi_password;
    doc["mqtt_server"] = config.mqtt_server;
    doc["mqtt_port"] = config.mqtt_port;
    doc["mqtt_username"] = config.mqtt_username;
    doc["mqtt_password"] = config.mqtt_password;

    String output;
    serializeJson(doc, output);
    // 原子写：WiFi/MQTT 配置写坏 = 设备连不上网、退 AP 模式，代价最高。
    // 以前这里直接 open("w")，写到一半掉电/空间不足就只剩半截 JSON。
    return Storage::saveFileAtomic(path, output);
}

bool NetworkConfigManager::loadFromFlash() {
    if (loadFromFile(CONFIG_FILE)) {
        return true;
    }

    // 主配置读不出来（文件损坏 / 写入中断电 / 首次启动无配置）→ 试备份
    if (!Storage::fileExists(BACKUP_FILE)) {
        return false;
    }
    Serial.println(F("[配置] 主配置不可用，尝试从备份恢复..."));
    if (loadFromFile(BACKUP_FILE)) {
        Serial.printf_P(PSTR("[配置] 备份可用（SSID: %s），已写回主配置\n"), config.wifi_ssid);
        // 注意：这里**不**走 saveToFlash()，否则 backupCurrentFile() 会把损坏的主配置
        // 覆盖成备份，好备份就没了。直接写主文件即可。
        writeConfigTo(CONFIG_FILE);
        return true;
    }
    Serial.println(F("[配置] 备份同样不可用"));
    return false;
}

bool NetworkConfigManager::saveToFlash() {
    // 只写主配置。备份由 markKnownGood() 在「真正连上 WiFi」时推进，
    // 不在这里动 —— 否则一次写错就会把已知良好的备份覆盖掉。
    return writeConfigTo(CONFIG_FILE);
}

String NetworkConfigManager::getBackupSSID() const {
    String content = Storage::readFile(BACKUP_FILE);
    if (content.length() == 0) {
        return String();
    }
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, content)) {
        return String();
    }
    return String((const char*)(doc["wifi_ssid"] | ""));
}

void NetworkConfigManager::markKnownGood() {
    // 联网成功后才把当前配置认定为「可用」。写失败无所谓（备份是可选保障）。
    if (strlen(config.wifi_ssid) == 0) {
        return;
    }
    if (writeConfigTo(BACKUP_FILE)) {
        Serial.printf_P(PSTR("[配置] 已更新已知良好配置备份（SSID: %s）\n"), config.wifi_ssid);
    }
}

bool NetworkConfigManager::hasBackup() const {
    return Storage::fileExists(BACKUP_FILE);
}

bool NetworkConfigManager::restoreFromBackup() {
    if (!Storage::fileExists(BACKUP_FILE)) {
        return false;
    }
    if (!loadFromFile(BACKUP_FILE)) {
        return false;
    }
    // 与启动自愈同理：不走 saveToFlash()，避免把损坏/错误的主配置反过来盖掉好备份
    return writeConfigTo(CONFIG_FILE);
}

