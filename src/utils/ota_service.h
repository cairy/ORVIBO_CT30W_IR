#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#include <Arduino.h>
#include <ESP8266WebServer.h>
#include "config/constants.h"

// OTA 无线升级服务（两种模式共用）
//   - 主通道 ArduinoOTA（TCP 8266）：PC 用 `pio run -t upload --upload-port <设备IP>` 直刷，
//     与 Web 服务的启停策略完全解耦 —— 这是本项目必须走它的原因：业务 Web 服务在
//     「MQTT 已连 + HA Discovery 已发布」的稳态下会被 main.cpp 主动停掉，浏览器上传路径不可达。
//   - 备用通道 ESP8266HTTPUpdateServer（浏览器 /update）：仅当调用方给了 Web 服务实例时挂载。
//
// 注意：两个通道共用 OTA_USERNAME / OTA_PASSWORD。
class OtaService {
public:
    // server 传 nullptr 表示只起 ArduinoOTA（安全模式常用，Web 服务自己管）
    void begin(ESP8266WebServer* server, const char* hostname = OTA_HOSTNAME);

    // 必须每次主循环调用（ArduinoOTA 只有被轮询才会接收）
    void handle();

    // 上传进行中 —— 调用方可据此跳过其它任务，避免 TCP 缓冲溢出导致上传失败
    bool isUploading() const { return _uploading; }

    void printStatus() const;

private:
    bool _uploading = false;
    bool _started = false;

    void _printSpace() const;
};

#endif // OTA_SERVICE_H
