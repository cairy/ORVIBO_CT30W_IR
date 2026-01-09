#include "mqtt_client.h"

MQTTClient::MQTTClient(NetworkConfigManager& config, WiFiClient& wifiClient)
    : client(wifiClient), configManager(config), lastReconnectAttempt(0) {
}

bool MQTTClient::init() {
    // 设置 MQTT 缓冲区大小为 2048 字节（用于支持较大的 Discovery 消息）
    client.setBufferSize(MQTT_BUFFER_SIZE);
    
    if (configManager.isValid()) {
        NetworkConfig& config = configManager.getConfig();
        client.setServer(config.mqtt_server, config.mqtt_port);
    }
    return true;
}

void MQTTClient::update() {
    if (!client.connected()) {
        unsigned long now = millis();
        if (now - lastReconnectAttempt > 5000) {
            lastReconnectAttempt = now;
            attemptReconnect();
        }
    } else {
        client.loop();
    }
}

bool MQTTClient::isConnected() {
    return client.connected();
}

bool MQTTClient::connect() {
    if (client.connected()) {
        return true;
    }
    
    // 检查可用内存
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 2048) {  // 至少需要2KB空闲内存
        return false;
    }
    
    String clientId = "ORVIBO-CT30W-" + String(ESP.getChipId());
    String statusTopic = String(MQTT_TOPIC_STATE) + "/status";
    
    NetworkConfig& config = configManager.getConfig();
    bool connected;
    if (strlen(config.mqtt_username) > 0) {
        connected = client.connect(clientId.c_str(), config.mqtt_username, config.mqtt_password, 
                                   statusTopic.c_str(), 0, true, "offline");
    } else {
        connected = client.connect(clientId.c_str());
    }
    
    if (connected) {
        // 发布在线状态（检查内存后再发布）
        if (ESP.getFreeHeap() >= 1024) {
            publish(statusTopic, "online", true);
        }
    }
    
    return connected;
}

void MQTTClient::disconnect() {
    client.disconnect();
}

bool MQTTClient::publish(const String& topic, const String& payload, bool retain) {
    if (!client.connected()) {
        return false;
    }
    
    // 检查内存和消息大小
    size_t freeHeap = ESP.getFreeHeap();
    size_t messageSize = topic.length() + payload.length();
    
    // 确保有足够的内存（消息大小 + 缓冲区开销 + 安全缓冲）
    if (freeHeap < messageSize + 512 || messageSize > MQTT_BUFFER_SIZE) {
        return false;
    }
    
    const char* topicStr = topic.c_str();
    const char* payloadStr = payload.c_str();
    
    if (topicStr == nullptr || payloadStr == nullptr) {
        return false;
    }
    
    return client.publish(topicStr, payloadStr, retain);
}

bool MQTTClient::subscribe(const String& topic) {
    if (!client.connected()) {
        return false;
    }
    return client.subscribe(topic.c_str());
}

void MQTTClient::setCallback(MQTT_CALLBACK_SIGNATURE) {
    client.setCallback(callback);
}

void MQTTClient::setServer(const String& server, uint16_t port) {
    client.setServer(server.c_str(), port);
}

void MQTTClient::setCredentials(const String& username, const String& password) {
    // PubSubClient库在connect时传递凭据，这里只保存
    // 实际凭据在connect()方法中使用
}

void MQTTClient::setBufferSize(uint16_t size) {
    client.setBufferSize(size);
}

bool MQTTClient::attemptReconnect() {
    if (!configManager.isValid()) {
        return false;
    }
    
    return connect();
}

