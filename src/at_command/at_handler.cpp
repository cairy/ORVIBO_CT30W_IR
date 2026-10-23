#include "at_handler.h"
#include <ESP8266WiFi.h>
#include <Esp.h>       // ESP.restart()
#include <string.h>

ATHandler::ATHandler(NetworkConfigManager& config)
    : configManager(config) {
}

void ATHandler::init() {
    commandBuffer = "";
    commandBuffer.reserve(COMMAND_MAX_LEN);
    commandOverflow = false;
}

void ATHandler::update() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (commandOverflow) {
                // 这一行已经超长：丢弃，并且**明确报错**。
                // 不能拿被截断的半条命令去执行 —— 那会变成"看起来配成功了、
                // 实际写进去半截"的静默错配。
                sendResponse(F("ERROR: command too long (max 128 bytes)"));
                commandOverflow = false;
            } else if (commandBuffer.length() > 0) {
                processCommand(commandBuffer);
            }
            commandBuffer = "";
            continue;
        }
        if (commandOverflow) continue;     // 已经超长，继续吞到行尾为止
        if (commandBuffer.length() >= COMMAND_MAX_LEN) {
            commandOverflow = true;
            commandBuffer = "";
            continue;
        }
        commandBuffer += c;
    }
}

void ATHandler::processCommand(const String& command) {
    String trimmed = command;
    trimmed.trim();

    // ⚠️ 参数必须取自**原始**字符串，不能取自大写后的副本。
    // 历史实现在这里把整条命令 toUpperCase() 以后再 substring(8) 取参数，
    // 结果 AT+WIFI= / AT+MQTT= 的 **SSID 与密码被强制大写**
    // （示意：`Abc@1234xy` → `ABC@1234XY`），含小写字母的密码永远配不上 WiFi，
    // 且失败后设备会退到 AP 配网模式，只能靠网页救回来。
    // 现在只对"命令名"做忽略大小写比较，参数保持原样。
    String upper = trimmed;
    upper.toUpperCase();

    if (upper.startsWith("AT+WIFI=")) {
        handleWiFi(trimmed.substring(8));
    } else if (upper.startsWith("AT+MQTT=")) {
        String params = trimmed.substring(8);
        params.trim();
        if (params.length() == 0) {
            // 空参数 = 清掉 MQTT 配置（等价于配网页里把「MQTT 服务器」留空）
            configManager.clearMQTT();
            sendResponse(configManager.save() ? String(F("OK: MQTT cleared")) : String(F("ERROR: save failed")));
        } else {
            handleMQTT(params);
        }
    } else if (upper == "AT+RESTORE") {
        handleRestore();
    } else if (upper == "AT+QUERY") {
        handleQuery();
    } else if (upper == "AT") {
        sendResponse(F("OK"));
    } else {
        sendResponse(F("ERROR"));
    }
}

void ATHandler::handleWiFi(const String& params) {
    String ssid, password;

    // ── 分隔符：SSID 与密码里都可能出现逗号，所以不能无脑取第一个逗号 ──
    // 以前的 `indexOf(',')` 遇到 `AT+WIFI=My,Net,pass` 会配成 SSID="My" / 密码="Net,pass"，
    // 而且照样回 OK —— 设备连不上、退进 AP 模式，用户却以为配置成功了。
    // 现在的规则（宁可报错也不静默错配）：
    //   · SSID 用双引号包住 ⇒ 引号内原样取，引号后紧跟的 ',' 之后全是密码
    //     （密码里可以有逗号）；写法：AT+WIFI="My,Net",pa,ss
    //   · 没有引号 ⇒ 只接受**恰好一个**逗号；两个以上直接报错并教怎么写
    if (params.length() > 0 && params.charAt(0) == '"') {
        int closeQuote = params.indexOf('"', 1);
        if (closeQuote < 0) {
            sendResponse(F("ERROR: SSID 的引号没有闭合。写法：AT+WIFI=\"我的,网络\",密码"));
            return;
        }
        ssid = params.substring(1, closeQuote);
        String rest = params.substring(closeQuote + 1);
        if (rest.length() > 0) {
            if (rest.charAt(0) != ',') {
                sendResponse(F("ERROR: SSID 引号后必须是逗号。写法：AT+WIFI=\"我的,网络\",密码"));
                return;
            }
            password = rest.substring(1);
        }
    } else {
        int first = params.indexOf(',');
        if (first < 0) {
            sendResponse(F("ERROR: Invalid format. Use AT+WIFI=SSID,PASSWORD"));
            return;
        }
        if (params.indexOf(',', first + 1) >= 0) {
            sendResponse(F("ERROR: SSID 或密码里含逗号，无法判断怎么切。请给 SSID 加引号：AT+WIFI=\"我的,网络\",密码"));
            return;
        }
        ssid = params.substring(0, first);
        password = params.substring(first + 1);
    }

    ssid.trim();
    // 长度校验走与配网页同一个函数（NetworkConfigManager::checkWiFi）：
    // 两个入口必须同一套判据，否则"AT 能过的网页过不了"这种差异会拖死人。
    String err = NetworkConfigManager::checkWiFi(ssid, password);
    if (err.length()) {
        sendResponse(String(F("ERROR: ")) + err);
        return;
    }

    configManager.setWiFi(ssid, password);
    sendResponse(configManager.save() ? String(F("OK")) : String(F("ERROR: save failed")));
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

        // ⚠️ `toInt()` 解析失败返回 0，而 0 以前被**直接写进配置**（配网页那条路径
        // 有 `port == 0 ⇒ 1883` 的兜底，两个入口行为不一致）。端口是 0 的 MQTT
        // 永远连不上，界面上却什么都看不出来。现在两处统一：非法就报错。
        String err = NetworkConfigManager::checkMQTT(server, port, username, password);
        if (err.length()) {
            sendResponse(String(F("ERROR: ")) + err);
            return;
        }

        configManager.setMQTT(server, port, username, password);
        sendResponse(configManager.save() ? String(F("OK")) : String(F("ERROR: save failed")));
    } else {
        sendResponse(F("ERROR: Invalid format. Use AT+MQTT=SERVER,PORT[,USERNAME,PASSWORD]"));
    }
}

void ATHandler::handleQuery() {
    NetworkConfig& config = configManager.getConfig();
    sendResponse(F("WiFi SSID: ") + String(config.wifi_ssid));
    sendResponse(F("WiFi password set: ") + String(strlen(config.wifi_password) > 0 ? "yes" : "no"));
    sendResponse(F("MQTT Server: ") + String(config.mqtt_server) + ":" + String(config.mqtt_port));
    sendResponse(String("WiFi mode: ") +
                 ((WiFi.getMode() & WIFI_AP) ? "AP" : ((WiFi.status() == WL_CONNECTED) ? "STA(connected)" : "STA(offline)")));

    // 备份（已知良好配置）—— 主配置填错、设备退进 AP 模式时，这就是救命的回退点
    if (configManager.hasBackup()) {
        String bak = configManager.getBackupSSID();
        sendResponse(F("Backup: yes, SSID=") + (bak.length() ? bak : String(String(F("(unreadable)")))));
    } else {
        sendResponse(F("Backup: no (never connected successfully)"));
    }

    sendResponse(String(F("Free heap: ")) + String(ESP.getFreeHeap()));
}

void ATHandler::handleRestore() {
    // 把「已知良好配置」恢复成主配置并重启。
    // 典型场景：改 MQTT / 配网时把 WiFi 写错，设备连不上退进 AP 模式，
    // 而本机连不上设备热点 ⇒ 用这条串口命令就能自救，不必人工拿手机配网。
    if (!configManager.hasBackup()) {
        sendResponse(F("ERROR: 没有可用备份（设备从未成功联网过）"));
        return;
    }
    String bak = configManager.getBackupSSID();
    if (!configManager.restoreFromBackup()) {
        sendResponse(F("ERROR: 备份恢复失败（备份文件已损坏）"));
        return;
    }
    NetworkConfig& config = configManager.getConfig();
    sendResponse(F("OK: 已恢复已知良好配置"));
    sendResponse("  SSID: " + String(config.wifi_ssid));
    sendResponse("  (恢复前备份中的 SSID: " + (bak.length() ? bak : String(String(F("(unreadable)")))) + ")");
    sendResponse(F("  正在重启以生效..."));
    delay(200);   // 让串口把字吐完
    ESP.restart();
}

void ATHandler::sendResponse(const String& response) {
    Serial.println(response);
}

