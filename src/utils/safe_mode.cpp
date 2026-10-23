#include "safe_mode.h"
#include "config/constants.h"
#include "config/pins.h"
#include "network/web_assets.h"

#include <ESP8266HTTPUpdateServer.h>

bool SafeMode::_active = false;

// 最近一次"有客户端来访问"的时刻，用来给安全模式页报一个真实的自动重启倒计时。
// 做成文件作用域静态变量：页面要显示它，而它只在 enter() 的主循环里被更新。
static unsigned long s_lastActivity = 0;
// 取倒计时那一次请求本身不该把空闲计时重置，否则页面永远显示满额时间
static bool s_skipActivityRefresh = false;

// ============================================================================
// RTC 计数（offset 必须 <= 63，见头文件说明）
// ============================================================================

bool SafeMode::_rtcRead(RtcBootData& data) {
    data.magic = 0;
    data.count = 0;
    data.unfinished = 0;
    data.checksum = 0;
    if (!ESP.rtcUserMemoryRead(RTC_OFFSET, reinterpret_cast<uint32_t*>(&data), sizeof(data))) {
        return false;
    }
    if (data.magic != RTC_MAGIC) {
        return false;
    }
    if (data.checksum != (data.magic ^ data.count ^ data.unfinished ^ 0xA5A5A5A5)) {
        return false;
    }
    return true;
}

bool SafeMode::_rtcWrite(const RtcBootData& data) {
    RtcBootData out = data;
    out.magic = RTC_MAGIC;
    out.checksum = out.magic ^ out.count ^ out.unfinished ^ 0xA5A5A5A5;
    return ESP.rtcUserMemoryWrite(RTC_OFFSET, reinterpret_cast<uint32_t*>(&out), sizeof(out));
}

uint32_t SafeMode::_readFailCount() {
    RtcBootData data;
    if (!_rtcRead(data) || data.count > 0xFFFFu) {
        return 0;   // 掉电/RTC 数据损坏都当作"没有失败记录"
    }
    return data.count;
}

void SafeMode::_clearFailCount() {
    RtcBootData data;
    data.magic = RTC_MAGIC;
    data.count = 0;
    data.unfinished = 0;
    _rtcWrite(data);
}

// ============================================================================
// 业务模式侧
// ============================================================================

void SafeMode::printBanner() {
    Serial.printf_P(PSTR("[固件] 版本 %s | 编译 %s %s\n"), FW_VERSION, __DATE__, __TIME__);
    Serial.printf_P(PSTR("[固件] Sketch MD5: %s\n"), ESP.getSketchMD5().c_str());
}

// 只有"上一次启动崩了"或"上一次启动没走完 setup()"才算失败。判据与理由见头文件。
uint32_t SafeMode::noteBoot(bool lastBootCrashed) {
    RtcBootData data;
    const bool valid = _rtcRead(data);
    const uint32_t prevCount = valid ? data.count : 0;
    const bool prevUnfinished = valid && (data.unfinished != 0);

    const bool suspicious = lastBootCrashed || prevUnfinished;
    uint32_t count = suspicious ? (prevCount + 1) : 0;
    if (count > 0xFFFFu) {
        count = SAFE_MODE_MAX_FAILS;   // 数据异常时按"已达阈值"处理，宁可进一次安全模式
    }

    data.magic = RTC_MAGIC;
    data.count = count;
    data.unfinished = 1;   // 本次尚未走到 setup() 末尾；setupCompleted() 会清掉
    if (!_rtcWrite(data)) {
        Serial.println(F("[安全模式] 警告: RTC 内存写入失败，启动失败计数无法持久化"));
        count = 0;
    }

    if (count > 0) {
        Serial.printf_P(PSTR("[安全模式] 可疑启动计数 %u/%u (%s)\n"),
                        (unsigned)count, (unsigned)SAFE_MODE_MAX_FAILS,
                        lastBootCrashed ? "上次崩溃/看门狗复位"
                                        : "上次未走完 setup()");
    }
    return count;
}

void SafeMode::setupCompleted() {
    RtcBootData data;
    data.magic = RTC_MAGIC;
    data.count = 0;
    data.unfinished = 0;
    if (!_rtcWrite(data)) {
        Serial.println(F("[安全模式] 警告: RTC 写入失败，完成标记无法持久化"));
    }
}

uint32_t SafeMode::getBootFailCount() {
    return _readFailCount();
}

// ============================================================================
// 进入判定
// ============================================================================

void SafeMode::_blinkConfirm() {
    pinMode(LED_PIN, OUTPUT);
    for (int i = 0; i < 3; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(80);
        digitalWrite(LED_PIN, LOW);
        delay(80);
    }
}

bool SafeMode::shouldEnter() {
    // ---- 通道 1：自动（连续多次启动未走到 setup() 末尾）----
    uint32_t fails = _readFailCount();
    if (fails >= SAFE_MODE_MAX_FAILS) {
        Serial.printf_P(PSTR("[安全模式] 连续 %u 次启动未完成，自动进入\n"), fails);
        _blinkConfirm();
        return true;
    }

    // ---- 通道 2：手动（上电按住 GPIO4）----
    // 零延迟设计：引脚没被按下就立刻返回，不给正常开机增加等待。
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    bool pressed = true;
    for (uint8_t i = 0; i < 3; i++) {
        delay(10);
        if (digitalRead(BUTTON_PIN) != LOW) {
            pressed = false;
            break;
        }
    }
    if (!pressed) {
        return false;
    }

    // 已按住：等够确认时长，期间 LED 快闪提示"识别到了，可以松手"
    pinMode(LED_PIN, OUTPUT);
    unsigned long startMs = millis();
    bool held = true;
    while (millis() - startMs < SAFE_MODE_CONFIRM_MS) {
        delay(20);
        if (digitalRead(BUTTON_PIN) != LOW) {
            held = false;
            break;
        }
        digitalWrite(LED_PIN, ((millis() - startMs) / 150) % 2 ? HIGH : LOW);
    }
    digitalWrite(LED_PIN, LOW);

    if (!held) {
        Serial.println(F("[安全模式] GPIO4 按下时间不足，按正常模式启动"));
        return false;
    }

    Serial.println(F("[安全模式] 手动触发：上电按住 GPIO4 已达确认时长"));
    _blinkConfirm();
    return true;
}

// ============================================================================
// 安全模式主流程
// ============================================================================

// 安全模式页：直接发"编进固件里的 gzip 资源"（web/safe.html）。
//
// 以前这里是用 String 拼出来的纯文本页（白底黑字 + margin:2em），三个毛病：
//   1. 手机上排版难看，也没有深色适配；
//   2. 外观跟常驻面板拉不开差距 —— 用户看不出"设备现在不是正常状态"；
//   3. 改一次页面要动 C++ 字符串。
// 现在它跟面板、配网页共用一套配色变量（web/theme.css），整页琥珀色警示带，
// 与常驻面板的绿色"运行中"横幅一眼可分。
//
// 页面里的动态信息（固件版本 / MD5 / 联网方式 / 倒计时）走 /safe-status，
// 静态部分仍是一次请求拿全 —— 救援页面不能依赖"第二个请求也能成功"。
void SafeMode::_serveRootPage(ESP8266WebServer& server) {
    server.sendHeader(F("Content-Encoding"), F("gzip"));
    server.sendHeader(F("Cache-Control"), F("no-cache"));
    server.send_P(200, WEB_SAFE_HTML_TYPE, (PGM_P)WEB_SAFE_HTML, WEB_SAFE_HTML_LEN);
}

[[noreturn]] void SafeMode::enter() {
    _active = true;
    _clearFailCount();   // 已经占用了这次机会，别让下一次重启再进来

    Serial.println(F("\n====== 安全模式 ======"));
    Serial.println(F("[安全模式] 仅加载：串口 + 联网 + 固件上传"));
    Serial.printf_P(PSTR("[安全模式] 自愈超时: %u 秒后自动重启回正常模式\n"),
                    (unsigned)(SAFE_MODE_IDLE_REBOOT_MS / 1000));
    printBanner();

    // ---- 1. 文件系统（只读意图，绝不自动格式化）----
    bool fsOk = false;
    LittleFSConfig cfg = LittleFSConfig(false);   // 与业务模式相反：这里不许格式化
    if (LittleFS.setConfig(cfg)) {
        fsOk = LittleFS.begin();
    }
    Serial.printf_P(PSTR("[安全模式] 文件系统: %s\n"), fsOk ? "已挂载" : "挂载失败（跳过）");

    // ---- 2. 联网 ----
    NetworkConfigManager net;
    String howToReach;
    bool wifiOk = false;

    if (fsOk) {
        net.load();
    }

    if (net.isValid()) {
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.begin(net.getConfig().wifi_ssid, net.getConfig().wifi_password);
        Serial.printf_P(PSTR("[安全模式] 连接 WiFi %s "), net.getConfig().wifi_ssid);
        unsigned long startMs = millis();
        while (millis() - startMs < SAFE_MODE_WIFI_WAIT_MS) {
            if (WiFi.status() == WL_CONNECTED) {
                wifiOk = true;
                break;
            }
            delay(250);
            Serial.print('.');
        }
        Serial.println();
        if (wifiOk) {
            howToReach = F("已连 WiFi, IP ") + WiFi.localIP().toString();
        }
    }

    if (!wifiOk) {
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASSWORD);
        howToReach = F("热点 ") + String(AP_SSID) + ", IP 192.168.4.1";
    }
    Serial.printf_P(PSTR("[安全模式] %s\n"), howToReach.c_str());

    // ---- 3. 上传服务：ArduinoOTA（主）+ /update（备）----
    ESP8266WebServer server(80);
    OtaService ota;
    ota.begin(&server, OTA_HOSTNAME);

    server.on("/", HTTP_GET, [&]() {
        _serveRootPage(server);
    });
    // 安全模式页的动态信息（固件身份、联网方式、升级空间、自动重启倒计时）。
    // 手拼 JSON 而不用 ArduinoJson：这一处字段极少且全是受控文本，
    // 省一次堆分配就少一个救援页面自己出问题的机会。
    server.on("/safe-status", HTTP_GET, [&]() {
        unsigned long idleMs = millis() - s_lastActivity;
        long leftSec = (long)(SAFE_MODE_IDLE_REBOOT_MS / 1000) - (long)(idleMs / 1000);
        if (leftSec < 0) {
            leftSec = 0;
        }
        String json;
        json.reserve(320);
        json = F("{\"success\":true,\"data\":{\"firmware\":\"");
        json += FW_VERSION;
        json += F("\",\"sketchMD5\":\"");
        json += ESP.getSketchMD5();
        json += F("\",\"fsOk\":");
        json += fsOk ? F("true") : F("false");
        json += F(",\"freeSketchSpace\":");
        json += String(ESP.getFreeSketchSpace());
        json += F(",\"rebootInSec\":");
        json += String(leftSec);
        json += F(",\"howToReach\":\"");
        json += howToReach;      // 受控文本（"已连 WiFi, IP x.x.x.x" / "热点 ORVIBO-CT30W…"）
        json += F("\"}}");
        server.send(200, F("application/json"), json);
        s_skipActivityRefresh = true;   // 取倒计时这件事本身不算"用户还在操作"
    });
    server.on("/exit", HTTP_GET, [&]() {
        server.send(200, F("text/html"), F("<meta http-equiv='refresh' content='3;url=/'><p>已清零启动计数，正在重启回正常模式…</p>"));
        delay(200);
        ESP.restart();
    });
    server.onNotFound([&]() {
        server.send(404, F("text/plain"), F("Not Found"));
    });
    server.begin();

    Serial.println(F("[安全模式] 就绪 —— 打开 /update 上传固件，或用命令行:"));
    Serial.printf_P(PSTR("[安全模式] pio run -t upload --upload-port %s\n"),
                    WiFi.localIP().toString().c_str());

    // ---- 4. 主循环（注意：这里再也不跑业务 loop 与 buttonHandler）----
    s_lastActivity = millis();
    while (true) {
        const bool hadClient = (bool)server.client();
        server.handleClient();
        if (hadClient && !s_skipActivityRefresh) {
            s_lastActivity = millis();
        }
        s_skipActivityRefresh = false;

        ota.handle();
        if (ota.isUploading()) {
            s_lastActivity = millis();          // 上传中不算空闲
        } else if (millis() - s_lastActivity > SAFE_MODE_IDLE_REBOOT_MS) {
            Serial.println(F("[安全模式] 长时间无操作，自动重启回正常模式"));
            delay(200);
            LittleFS.end();
            ESP.restart();
        }

        // LED 慢闪 1Hz，和安全模式之外的状态明显区分
        digitalWrite(LED_PIN, (millis() % 1000) < 500 ? LOW : HIGH);

        yield();
        ESP.wdtFeed();
        delay(10);
    }
}
