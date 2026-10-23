#include "ota_service.h"
#include "config/constants.h"

#include <ArduinoOTA.h>
#include <ESP8266HTTPUpdateServer.h>

void OtaService::_printSpace() const {
    // 判定式见 Updater.cpp: `round4k(新镜像) <= FS_start - round4k(当前固件)`
    size_t freeSpace = ESP.getFreeSketchSpace();
    Serial.printf_P(PSTR("[OTA] 可用暂存: %u B, 当前空闲堆: %u B\n"),
                    freeSpace, ESP.getFreeHeap());
    // ESP8266HTTPUpdateServer 实际以 (freeSpace - 0x1000) & 0xFFFFF000 起刷
    size_t maxFile = (freeSpace - 0x1000) & 0xFFFFF000;
    Serial.printf_P(PSTR("[OTA] 本次最大可上传文件: %u B (固件 %s)\n"), maxFile, FW_VERSION);
    if (freeSpace < 2 * ESP.getSketchSize()) {
        Serial.println(F("[OTA] 警告: 空间不足以来回反复刷同尺寸固件，后续请勿再增肥固件"));
    }
}

void OtaService::printStatus() const {
    // 不打印主机名：mDNS 已禁用（见 begin() 里的说明），主机名不可解析，印出来只会误导
    Serial.printf_P(PSTR("[OTA] 状态: %s, 端口 %d (espota 请用 IP 连接)\n"),
                    _started ? "已启动" : "未启动", OTA_PORT);
}

void OtaService::begin(ESP8266WebServer* server, const char* hostname) {
    if (_started) {
        return;
    }

    Serial.println(F("=== OTA 无线升级服务 ==="));
    _printSpace();

    // ---------- 备用通道：浏览器上传 ----------
    if (server != nullptr) {
        // 静态生命周期：handler 会长期持有该对象指针
        static ESP8266HTTPUpdateServer httpUpdater(true);  // true = 输出串口调试
        httpUpdater.setup(server, "/update", OTA_USERNAME, OTA_PASSWORD);
        Serial.println(F("[OTA] 浏览器上传: http://<设备IP>/update (需认证)"));
    }

    // ---------- 主通道：ArduinoOTA ----------
    // 🔴 mDNS 必须关掉，而且**只有传 false 才真的关**：
    //   ArduinoOTA.begin(bool useMDNS = true)  → 默认值会在内部执行 MDNS.begin(_hostname)，
    //   所以"只删掉 setHostname()"是没用的（v1.3.0~1.3.3 就栽在这里，白挨了好几轮崩溃）。
    // 为什么本机扛不住 mDNS：总堆仅约 18.5KB（静态 RAM 已占 61.7KB/80KB），联网后空闲只有 ~3KB；
    // 收到一次 mDNS 查询它就要分配 540 字节。
    // 实测（2026-09-26，v1.3.3）：`last failed alloc call: 402381F2(540)` 反解正是
    // `MDNSResponder::_readRRAnswer` → 分配失败 → "Unhandled C++ exception: OOM" → 崩溃重启。
    // espota 一直是按 IP 直连的（`-i <设备IP>`），mDNS / `.local` 域名对本案毫无用处。
    // 代价：不能用 `.local` 域名访问设备，只能记 IP。
    // 如果要恢复 .local 访问，必须先解决内存（见 docs/ui-experience-plan.md 的内存瘦身项）。
    (void)hostname;
    ArduinoOTA.setPort(OTA_PORT);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([this]() {
        _uploading = true;
        String type = (ArduinoOTA.getCommand() == U_FLASH) ? F("固件") : F("文件系统");
        Serial.printf_P(PSTR("\n[OTA] 开始写入 %s...\n"), type.c_str());
        if (ArduinoOTA.getCommand() != U_FLASH) {
            // U_FS 会连带卸载 LittleFS，业务模式下的配置读写会随之失效，
            // 本项目没有做 FS 的热重挂，刷完文件系统后请手动重启一次。
            Serial.println(F("[OTA] 警告: 文件系统刷写会让 LittleFS 被卸载，完成后必须重启"));
        }
    });

    ArduinoOTA.onEnd([this]() {
        Serial.println(F("\n[OTA] 写入完成，设备即将重启"));
        _uploading = false;
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        static unsigned int lastPercent = 0;
        unsigned int percent = total ? (progress * 100 / total) : 0;
        if (percent / 5 != lastPercent / 5) {   // 每 5% 打印一次，避免刷屏
            Serial.printf_P(PSTR("[OTA] %u%%\n"), percent);
        }
        lastPercent = percent;
    });

    ArduinoOTA.onError([this](ota_error_t error) {
        _uploading = false;
        Serial.printf_P(PSTR("[OTA] 失败(错误码 %u): "), error);
        if (error == OTA_AUTH_ERROR) {
            Serial.println(F("认证失败 —— 检查 OTA_PASSWORD 是否与 PC 端一致"));
        } else if (error == OTA_BEGIN_ERROR) {
            Serial.println(F("空间不足 —— 固件放不下 [FS_start - 当前固件] 之间"));
        } else if (error == OTA_CONNECT_ERROR) {
            Serial.println(F("连接失败 —— 多半是 PC 侧防火墙挡住了设备回连"));
        } else if (error == OTA_RECEIVE_ERROR) {
            Serial.println(F("接收失败 —— 多半是上传期间被打断"));
        } else if (error == OTA_END_ERROR) {
            Serial.println(F("结束校验失败 —— 镜像首字节不是 0xE9 或长度不对"));
        } else {
            Serial.println(F("未知错误"));
        }

        // ---- 兜底自愈（实测踩过的坑，别删）----
        // 一旦 ArduinoOTA 走到 begin 之后失败，Updater 会停在"_size > 0"的残留状态，
        // 而 Updater::begin() 见到 _size>0 就直接返回 false —— 于是浏览器 /update 那条备用通道
        // 也会跟着一起失效，一直卡到下次重启为止。
        // 更糟的是：这时候如果哪条通道真把 end() 走完，会把没写完的半截镜像标记成启动目标。
        // 这里主动重启把状态清干净：此时还没下发 eboot copy 命令，重启只会回到原固件，是安全的。
        if (Update.isRunning()) {
            Serial.println(F("[OTA] 升级状态有残留，重启以恢复（当前固件不受影响）"));
            delay(1500);
            ESP.restart();
        }
    });

    // 关键：false = 不启用 mDNS（理由见上方注释）。espota 用 -i <设备IP> 直连，不需要它。
    ArduinoOTA.begin(false);
    _started = true;
    printStatus();
}

void OtaService::handle() {
    if (_started) {
        ArduinoOTA.handle();
    }
}
