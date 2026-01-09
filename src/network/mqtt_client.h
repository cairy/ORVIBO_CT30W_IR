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
    bool subscribe(const String& topic);
    void setCallback(MQTT_CALLBACK_SIGNATURE);
    void setServer(const String& server, uint16_t port);
    void setCredentials(const String& username, const String& password);
    void setBufferSize(uint16_t size);

private:
    PubSubClient client;
    NetworkConfigManager& configManager;
    unsigned long lastReconnectAttempt;
    bool attemptReconnect();
};

#endif // MQTT_CLIENT_H

