#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include "network_config.h"
#include "config/constants.h"

// WiFi 连接状态
enum WiFiStatus {
    WIFI_DISCONNECTED,
    WIFI_AP_MODE,
    WIFI_CONNECTING,
    WIFI_CONNECTED
};

// WiFi 管理器
class WiFiManager {
public:
    WiFiManager(NetworkConfigManager& config);
    bool init();
    void update();
    WiFiStatus getStatus() const;
    bool isConnected() const;
    String getIP() const;
    void startAP();
    void stopAP();
    bool connect(const String& ssid, const String& password);
    void disconnect();

private:
    NetworkConfigManager& configManager;
    WiFiStatus status;
    unsigned long lastReconnectAttempt;
    int reconnectAttempts;
    bool apMode;
    
    bool attemptReconnect();
    void handleReconnect();
};

#endif // WIFI_MANAGER_H

