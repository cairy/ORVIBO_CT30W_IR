#include "wifi_manager.h"

WiFiManager::WiFiManager(NetworkConfigManager& config) 
    : configManager(config), status(WIFI_DISCONNECTED), 
      lastReconnectAttempt(0), reconnectAttempts(0), apMode(false) {
}

bool WiFiManager::init() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    
    if (configManager.isValid()) {
        NetworkConfig& config = configManager.getConfig();
        return connect(config.wifi_ssid, config.wifi_password);
    } else {
        startAP();
        return true;
    }
}

void WiFiManager::update() {
    if (apMode) {
        return;  // AP模式下不需要更新
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        if (status != WIFI_CONNECTED) {
            status = WIFI_CONNECTED;
            reconnectAttempts = 0;
            Serial.println("WiFi connected");
        }
    } else {
        if (status == WIFI_CONNECTED) {
            status = WIFI_DISCONNECTED;
            Serial.println("WiFi disconnected");
        }
        handleReconnect();
    }
}

WiFiStatus WiFiManager::getStatus() const {
    return status;
}

bool WiFiManager::isConnected() const {
    return (status == WIFI_CONNECTED && WiFi.status() == WL_CONNECTED);
}

String WiFiManager::getIP() const {
    if (apMode) {
        return WiFi.softAPIP().toString();
    }
    return WiFi.localIP().toString();
}

void WiFiManager::startAP() {
    WiFi.mode(WIFI_AP);
    
    // 配置AP的IP地址、网关和子网掩码
    IPAddress apIP(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(apIP, gateway, subnet);
    
    // 启动AP模式
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    
    apMode = true;
    status = WIFI_AP_MODE;
    
    // 等待AP启动完成
    delay(100);
    
    // 安全地获取IP地址字符串（避免临时对象问题）
    String ipStr = WiFi.softAPIP().toString();
    Serial.print("AP mode started: SSID=");
    Serial.print(AP_SSID);
    Serial.print(", IP=");
    Serial.println(ipStr);
}

void WiFiManager::stopAP() {
    WiFi.softAPdisconnect(true);
    apMode = false;
    WiFi.mode(WIFI_STA);
}

bool WiFiManager::connect(const String& ssid, const String& password) {
    stopAP();
    status = WIFI_CONNECTING;
    reconnectAttempts = 0;
    
    WiFi.begin(ssid.c_str(), password.c_str());
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        status = WIFI_CONNECTED;
        // 安全地获取IP地址字符串（避免临时对象问题）
        String ipStr = WiFi.localIP().toString();
        Serial.print("\nWiFi connected: ");
        Serial.println(ipStr);
        return true;
    } else {
        status = WIFI_DISCONNECTED;
        Serial.println("\nWiFi connection failed");
        startAP();
        return false;
    }
}

void WiFiManager::disconnect() {
    WiFi.disconnect();
    status = WIFI_DISCONNECTED;
}

bool WiFiManager::attemptReconnect() {
    if (!configManager.isValid()) {
        return false;
    }
    
    NetworkConfig& config = configManager.getConfig();
    WiFi.begin(config.wifi_ssid, config.wifi_password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) {
        delay(500);
        attempts++;
    }
    
    return (WiFi.status() == WL_CONNECTED);
}

void WiFiManager::handleReconnect() {
    unsigned long now = millis();
    
    if (now - lastReconnectAttempt < WIFI_RECONNECT_DELAY_MS) {
        return;
    }
    
    lastReconnectAttempt = now;
    
    if (reconnectAttempts >= WIFI_RECONNECT_MAX_ATTEMPTS) {
        Serial.println("Max reconnect attempts reached, starting AP mode");
        startAP();
        reconnectAttempts = 0;
        return;
    }
    
    reconnectAttempts++;
    Serial.printf("Attempting WiFi reconnect (%d/%d)...\n", 
                  reconnectAttempts, WIFI_RECONNECT_MAX_ATTEMPTS);
    
    if (attemptReconnect()) {
        status = WIFI_CONNECTED;
        reconnectAttempts = 0;
        Serial.println("WiFi reconnected");
    } else {
        status = WIFI_DISCONNECTED;
    }
}

