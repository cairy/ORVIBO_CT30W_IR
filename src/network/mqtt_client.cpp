#include "mqtt_client.h"

MQTTClient::MQTTClient(NetworkConfigManager& config, WiFiClient& wifiClient)
    : client(wifiClient), configManager(config), lastReconnectAttempt(0), reconnectAttempts(0),
      bufferReady(false) {
}

bool MQTTClient::init() {
    // MQTT 收发缓冲（8 KB）改为**按需分配**：只有真的配了 MQTT 才占这 8 KB。
    // 本机空闲堆总共只有几 KB，未配置 MQTT 时这块纯属白占（且是开机最早被拿走的）。
    ensureBuffer();

    if (configManager.isValid()) {
        NetworkConfig& config = configManager.getConfig();
        client.setServer(config.mqtt_server, config.mqtt_port);
    }
    return true;
}

// 按需分配 MQTT 收发缓冲；返回缓冲是否可用
// 未配置 MQTT ⇒ 不分配（返回 false，调用方据此跳过连接）
// 运行中才配上 MQTT（网页改配置）⇒ 这里补分配
bool MQTTClient::ensureBuffer() {
    if (bufferReady) {
        return true;
    }
    if (!configManager.isMQTTConfigured()) {
        return false;
    }

    if (!client.setBufferSize(MQTT_BUFFER_SIZE)) {
        return false;  // 内存不足；下次 update() 会重试
    }

    // 缓冲就绪的同时对齐服务器地址（支持运行中改配置）
    NetworkConfig& config = configManager.getConfig();
    client.setServer(config.mqtt_server, config.mqtt_port);
    bufferReady = true;

    Serial.printf_P(PSTR("[MQTT] 已分配收发缓冲 %u B（空闲堆 %u B）\n"),
                  (unsigned)MQTT_BUFFER_SIZE, (unsigned)ESP.getFreeHeap());
    return true;
}

void MQTTClient::update() {
    // 如果MQTT未配置，不尝试连接，也不占收发缓冲
    if (!configManager.isMQTTConfigured()) {
        return;
    }

    // 运行中才配上 MQTT：这时补分配缓冲；分配不出来就先不连
    if (!ensureBuffer()) {
        return;
    }
    
    if (!client.connected()) {
        unsigned long now = millis();
        unsigned long reconnectInterval = getReconnectInterval();
        if (now - lastReconnectAttempt > reconnectInterval) {
            lastReconnectAttempt = now;
            // 喂看门狗，避免连接过程中超时
            ESP.wdtFeed();
            if (attemptReconnect()) {
                reconnectAttempts = 0;  // 连接成功，重置计数
            } else {
                if (reconnectAttempts < MAX_RECONNECT_ATTEMPTS) {
                    reconnectAttempts++;  // 连接失败，增加计数
                }
            }
        }
    } else {
        reconnectAttempts = 0;  // 已连接，重置计数
        // 喂看门狗，避免loop()处理消息时超时
        ESP.wdtFeed();
        client.loop();
    }
}

// 指数退避重连间隔：5s, 10s, 20s, 40s, 60s（最大）
unsigned long MQTTClient::getReconnectInterval() const {
    if (reconnectAttempts == 0) return 5000;
    if (reconnectAttempts == 1) return 10000;
    if (reconnectAttempts == 2) return 20000;
    if (reconnectAttempts == 3) return 40000;
    return 60000;  // 最大60秒
}

bool MQTTClient::isConnected() {
    return client.connected();
}

bool MQTTClient::connect() {
    if (client.connected()) {
        return true;
    }

    // 未配置 MQTT 或缓冲分配不出来，都不连（也不占内存）
    if (!ensureBuffer()) {
        return false;
    }
    
    // 检查可用内存
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 2048) {  // 至少需要2KB空闲内存
        return false;
    }
    
    String clientId = "ORVIBO-CT30W-" + String(ESP.getChipId());
    String statusTopic = String(MQTT_TOPIC_STATE) + "/status";
    
    NetworkConfig& config = configManager.getConfig();
    // ⚠️ LWT 必须**无条件**下发。以前只有"配了用户名"那条分支带 will，
    // 而匿名 broker 恰恰是 Home Assistant + mosquitto 最常见的配置 ⇒
    // 这类设备上 state/status 永远收不到 "offline"，而 Discovery 里
    // binary_sensor/status 的 payload_off 就是 "offline"
    // ⇒ 设备掉电/崩溃后 HA 永远显示"在线"，所有掉线告警失效。
    // PubSubClient 的 connect() 允许 user/pass 传 nullptr（匿名）。
    const char* mqttUser = (strlen(config.mqtt_username) > 0) ? config.mqtt_username : nullptr;
    const char* mqttPass = (strlen(config.mqtt_password) > 0) ? config.mqtt_password : nullptr;
    bool connected = client.connect(clientId.c_str(), mqttUser, mqttPass,
                                    statusTopic.c_str(), 0, true, "offline");
    
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

bool MQTTClient::publish(const char* topic, const char* payload, bool retain) {
    if (topic == nullptr || payload == nullptr) {
        return false;
    }
    if (!client.connected()) {
        return false;
    }

    // 用 strlen 量长度，不构造 String —— 这一版的存在意义就是"零堆分配"
    size_t messageSize = strlen(topic) + strlen(payload);
    if (ESP.getFreeHeap() < messageSize + 512 || messageSize > MQTT_BUFFER_SIZE) {
        return false;
    }

    return client.publish(topic, payload, retain);
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
    bufferReady = client.setBufferSize(size);
}

bool MQTTClient::attemptReconnect() {
    if (!configManager.isValid()) {
        return false;
    }
    
    // 检查MQTT是否配置，未配置时不尝试连接
    if (!configManager.isMQTTConfigured()) {
        return false;
    }
    
    return connect();
}

