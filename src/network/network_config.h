#ifndef NETWORK_CONFIG_H
#define NETWORK_CONFIG_H

#include <Arduino.h>

// 网络配置结构体
struct NetworkConfig {
    char wifi_ssid[32];
    char wifi_password[64];
    char mqtt_server[64];
    uint16_t mqtt_port;
    char mqtt_username[32];
    char mqtt_password[64];
    bool valid;
};

// 网络配置管理器
class NetworkConfigManager {
public:
    NetworkConfigManager();
    bool load();
    bool save();
    NetworkConfig& getConfig();
    void setWiFi(const String& ssid, const String& password);
    void setMQTT(const String& server, uint16_t port, 
                const String& username, const String& password);
    void clearMQTT();
    bool isValid() const;
    bool isMQTTConfigured() const;
    void reset();

private:
    NetworkConfig config;
    static const char* CONFIG_FILE;
    bool loadFromFlash();
    bool saveToFlash();
};

#endif // NETWORK_CONFIG_H

