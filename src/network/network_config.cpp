#include "network_config.h"
#include "config/constants.h"
#include "utils/storage.h"
#include <ArduinoJson.h>

const char* NetworkConfigManager::CONFIG_FILE = CONFIG_FILE_NETWORK;

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

bool NetworkConfigManager::loadFromFlash() {
    String content = Storage::readFile(CONFIG_FILE);
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
    
    config.valid = (strlen(config.wifi_ssid) > 0);
    return config.valid;
}

bool NetworkConfigManager::saveToFlash() {
    DynamicJsonDocument doc(1024);
    doc["wifi_ssid"] = config.wifi_ssid;
    doc["wifi_password"] = config.wifi_password;
    doc["mqtt_server"] = config.mqtt_server;
    doc["mqtt_port"] = config.mqtt_port;
    doc["mqtt_username"] = config.mqtt_username;
    doc["mqtt_password"] = config.mqtt_password;
    
    String output;
    serializeJson(doc, output);
    return Storage::saveFile(CONFIG_FILE, output);
}

