#include "at_handler.h"

ATHandler::ATHandler(NetworkConfigManager& config)
    : configManager(config) {
}

void ATHandler::init() {
    commandBuffer = "";
}

void ATHandler::update() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (commandBuffer.length() > 0) {
                processCommand(commandBuffer);
                commandBuffer = "";
            }
        } else {
            commandBuffer += c;
        }
    }
}

void ATHandler::processCommand(const String& command) {
    String upperCommand = command;
    upperCommand.toUpperCase();
    
    if (upperCommand.startsWith("AT+WIFI=")) {
        String params = upperCommand.substring(8);
        handleWiFi(params);
    } else if (upperCommand.startsWith("AT+MQTT=")) {
        String params = upperCommand.substring(8);
        handleMQTT(params);
    } else if (upperCommand == "AT+QUERY") {
        handleQuery();
    } else if (upperCommand == "AT") {
        sendResponse("OK");
    } else {
        sendResponse("ERROR");
    }
}

void ATHandler::handleWiFi(const String& params) {
    int commaIndex = params.indexOf(',');
    if (commaIndex > 0) {
        String ssid = params.substring(0, commaIndex);
        String password = params.substring(commaIndex + 1);
        configManager.setWiFi(ssid, password);
        configManager.save();
        sendResponse("OK");
    } else {
        sendResponse("ERROR: Invalid format. Use AT+WIFI=SSID,PASSWORD");
    }
}

void ATHandler::handleMQTT(const String& params) {
    // Format: AT+MQTT=SERVER,PORT,USERNAME,PASSWORD
    // 简化版本：AT+MQTT=SERVER,PORT
    int comma1 = params.indexOf(',');
    if (comma1 > 0) {
        String server = params.substring(0, comma1);
        String rest = params.substring(comma1 + 1);
        int comma2 = rest.indexOf(',');
        
        uint16_t port = 1883;
        String username = "";
        String password = "";
        
        if (comma2 > 0) {
            port = rest.substring(0, comma2).toInt();
            String rest2 = rest.substring(comma2 + 1);
            int comma3 = rest2.indexOf(',');
            if (comma3 > 0) {
                username = rest2.substring(0, comma3);
                password = rest2.substring(comma3 + 1);
            } else {
                username = rest2;
            }
        } else {
            port = rest.toInt();
        }
        
        configManager.setMQTT(server, port, username, password);
        configManager.save();
        sendResponse("OK");
    } else {
        sendResponse("ERROR: Invalid format. Use AT+MQTT=SERVER,PORT[,USERNAME,PASSWORD]");
    }
}

void ATHandler::handleQuery() {
    NetworkConfig& config = configManager.getConfig();
    sendResponse("WiFi SSID: " + String(config.wifi_ssid));
    sendResponse("MQTT Server: " + String(config.mqtt_server) + ":" + String(config.mqtt_port));
}

void ATHandler::sendResponse(const String& response) {
    Serial.println(response);
}

