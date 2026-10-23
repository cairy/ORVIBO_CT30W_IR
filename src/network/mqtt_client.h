#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include "network_config.h"
#include "config/constants.h"

// MQTT 客户端封装
class MQTTClient {
public:
    MQTTClient(NetworkConfigManager& config, WiFiClient& wifiClient);
    bool init();
    void update();
    bool isConnected();
    bool connect();
    void disconnect();
    bool publish(const String& topic, const String& payload, bool retain = false);
    // const char* 版：给"栈上 char 缓冲 / PROGMEM 常量模板"用，避免为传参隐式构造 String
    // 再吃一次堆（本机配了 MQTT 时空闲堆只有 ~4 KB，任何一次多余分配都可能致命）。
    bool publish(const char* topic, const char* payload, bool retain = false);
    bool subscribe(const String& topic);
    void setCallback(MQTT_CALLBACK_SIGNATURE);
    void setServer(const String& server, uint16_t port);
    void setCredentials(const String& username, const String& password);
    void setBufferSize(uint16_t size);

private:
    PubSubClient client;
    NetworkConfigManager& configManager;
    unsigned long lastReconnectAttempt;
    uint8_t reconnectAttempts;           // 重连尝试次数（用于指数退避）
    bool bufferReady;                    // MQTT 收发缓冲是否已分配（按需分配，见 ensureBuffer）
    static const uint8_t MAX_RECONNECT_ATTEMPTS = 10;  // 最大重连尝试次数
    bool attemptReconnect();
    bool ensureBuffer();                 // 按需分配 MQTT 收发缓冲（未配置 MQTT 时不占）
    unsigned long getReconnectInterval() const;  // 获取当前重连间隔（指数退避）
};

#endif // MQTT_CLIENT_H

