#include "web_server.h"
#include "web_assets.h"
#include "network/wifi_manager.h"
#include "ir_core/ir_mapper.h"
#include "ir_core/ir_forwarder.h"
#include "ir_core/ir_learner.h"
#include "network/mqtt_client.h"
#include "utils/logger.h"
#include "utils/memory_monitor.h"
#include "utils/health_guard.h"
#include "utils/safe_mode.h"
#include "utils/net_watchdog.h"
#include "utils/crash_record.h"   // v1.3.50 崩溃自留档
#include "utils/hex_utils.h"
#include "utils/api_response.h"
#include "utils/api_handlers.h"
#include "utils/device_state.h"
#include "hardware/led_control.h"
#include "config/constants.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ESP8266WiFi.h>

// ledControl 为 main.cpp 中的全局对象，配网成功时用它做硬件确认（快闪）
extern LEDControl ledControl;

WebServer::WebServer(NetworkConfigManager& config)
    : server(80), configManager(config), running(false),
      wifiScanRunning(false), wifiScanStartedAt(0),
      mapper(nullptr), forwarder(nullptr), learner(nullptr), logger(nullptr),
      mqtt(nullptr) {
}

bool WebServer::guardRequest() {
    // ① Host 白名单：挡 DNS rebinding —— 攻击者让自己的域名解析到设备内网 IP，
    //    浏览器发来的请求 Host 是伪造域名，不在白名单里 ⇒ 403。
    if (!hostAllowed(server.hostHeader())) {
        server.send(403, "text/plain", "Forbidden");
        return false;
    }
    // ② 可选 Basic 认证（API_PASSWORD 为空 = 不启用，行为与 v1.3.18 及更早一致）
    if (API_PASSWORD[0] != '\0' && !server.authenticate(OTA_USERNAME, API_PASSWORD)) {
        server.requestAuthentication();
        return false;
    }
    return true;
}

void WebServer::onGuarded(const char* uri, HTTPMethod method, std::function<void()> fn) {
    server.on(uri, method, [this, fn]() {
        if (!guardRequest()) return;
        // 记一笔"最近一次收到并放行的请求"。它进僵死快照的时间线：
        // 重启后据此判断"从最后一次成功请求到彻底不通"隔了多久。
        NetWatchdog::noteHttpServed(millis());
        fn();
    });
}

bool WebServer::init() {
    // 面板（内置 SPA，编进固件，跟着 OTA 一起更新）
    //
    // ⚠️ 关于鉴权：全仓库此前只有 `/update`（刷固件，由 ESP8266HTTPUpdateServer
    // 内部实现）有认证，其余 20+ 个路由全是匿名可达 —— 包括改 WiFi/MQTT 并重启
    // （POST /save）、清空规则（PUT /api/maps 默认语义先删光再写）、任意发红外
    // （POST /api/signals/send）、恢复配置并重启（POST /api/restore）。
    // 也就是说：能访问到设备 IP（或连上配网热点）就能完整接管它。
    //
    // 现在这些都走 onGuarded()：Host 白名单（挡 DNS rebinding）+ 可选 Basic 认证。
    // 认证默认**不启用**（API_PASSWORD 留空），否则我的压测 / 长稳脚本、
    // 以及任何不带凭据的自动化都会 401。要启用就在 secrets.ini 填 api_password。
    // 浏览器在第一次打开面板时弹一次登录框，之后同源 fetch 自动带凭据，
    // 面板本身照常可用。
    onGuarded("/", HTTP_GET, [this]() { this->handleRoot(); });
    // 这里曾经还有 /app.css 与 /app.js 两条独立路由（gzip 后各 3.1 KB / 15.6 KB flash），
    // 保留理由是"防旧缓存页面来取"。该理由不成立：sendAsset_P() 一直就发
    // `Cache-Control: no-cache`，旧页面拿不到陈旧资源。两者已于 2026-09-28 删除。
    // 现在面板只有一次请求（/ 发内联版），见 handleRoot()。
    server.on("/favicon.ico", HTTP_GET, [this]() { server.send(204, "text/plain", ""); });

    // 配网页：同样走"内置资源 + gzip"，与面板共用一套配色变量（web/theme.css）。
    // 与旧版（在固件里拼 HTML 字符串）相比：页面不再受 RAM 限制，也能跟随系统深浅色，
    // 而且改网页只改 web/setup.html —— 与面板同一条流水线。
    server.on("/setup", HTTP_GET, [this]() { this->handleSetupPage(); });
    server.on("/config", HTTP_GET, [this]() { this->handleSetupPage(); });
    onGuarded("/save", HTTP_POST, [this]() { this->handleSave(); });
    // 配网时的辅助接口：搜索周边 WiFi（手机手敲 SSID 太容易错）、回退到已知良好配置。
    // 搜索是两段式（起扫 + 取结果），见 handleAPIWifiScan 的注释。
    onGuarded("/api/wifi/scan", HTTP_GET, [this]() { this->handleAPIWifiScan(); });
    onGuarded("/api/wifi/scan/result", HTTP_GET, [this]() { this->handleAPIWifiScanResult(); });
    onGuarded("/api/restore", HTTP_POST, [this]() { this->handleAPIRestore(); });

    // HTTP API路由（全部走 onGuarded —— 见上方 "/"，逐个注释太啰嗦）
    onGuarded("/api/status", HTTP_GET, [this]() { this->handleAPIStatus(); });
    onGuarded("/api/maps", HTTP_GET, [this]() { this->handleAPIMaps(); });
    onGuarded("/api/maps", HTTP_PUT, [this]() { this->handleAPIUpdateMap(); });
    onGuarded("/api/forwarding", HTTP_PUT, [this]() { this->handleAPISetForwarding(); });
    onGuarded("/api/forwarding", HTTP_POST, [this]() { this->handleAPISetForwarding(); });
    onGuarded("/api/logs", HTTP_GET, [this]() { this->handleAPILogs(); });
    onGuarded("/api/learn/start", HTTP_POST, [this]() { this->handleAPILearnStart(); });
    onGuarded("/api/learn/status", HTTP_GET, [this]() { this->handleAPILearnStatus(); });
    onGuarded("/api/learn/stop", HTTP_POST, [this]() { this->handleAPILearnStop(); });
    onGuarded("/api/learn/result", HTTP_GET, [this]() { this->handleAPILearnResult(); });
    // 清除学习结果（v1.3.50）：落盘的结果会一直留着，给用户一个手动清的入口
    onGuarded("/api/learn/result", HTTP_DELETE, [this]() { this->handleAPIClearLearnResult(); });
    onGuarded("/api/signals/recent", HTTP_GET, [this]() { this->handleAPIRecentSignals(); });
    // 清空最近信号（v1.3.24）—— 以前只有 GET，"清空"按钮挂的是转发记录那条，
    // 于是"最近信号"这一节在任何情况下都清不掉：Logger::clearRecentSequences()
    // 存在，但唯一调用点是 main.cpp 的堆见底软恢复，没有任何 HTTP 入口。
    onGuarded("/api/signals/recent", HTTP_DELETE, [this]() { this->handleAPIClearRecentSignals(); });

    onGuarded("/api/signals/send", HTTP_POST, [this]() { this->handleAPISendSignal(); });
    // 转发记录：回答"刚才那条信号到底转发没有、转到哪去了"
    onGuarded("/api/forward/records", HTTP_GET, [this]() { this->handleAPIGetForwardRecords(); });
    onGuarded("/api/forward/records", HTTP_DELETE, [this]() { this->handleAPIClearForwardRecords(); });
    onGuarded("/api/forward/test", HTTP_POST, [this]() { this->handleAPIForwardTest(); });
    // 设备 / 按钮配置（v1.3.10）—— 以前只存在浏览器 localStorage，换手机或清缓存就丢
    // ⭐ v1.3.42：写入改走**分片**（/api/devices/chunk）。整份 POST 在堆紧时会被框架
    //   直接丢连接（详情见 handleAPISaveDevicesChunk 头部注释）；`/api/devices` POST
    //   保留为小配置快路径与外部脚本兼容。
    onGuarded("/api/devices", HTTP_GET, [this]() { this->handleAPIGetDevices(); });
    onGuarded("/api/devices", HTTP_POST, [this]() { this->handleAPISaveDevices(); });
    onGuarded("/api/devices/chunk", HTTP_POST, [this]() { this->handleAPISaveDevicesChunk(); });

    // 网络僵死现场（v1.3.36）—— 设备因网络僵死自愈重启后，现场快照从这里取回。
    // 面板暂不展示，排障时直接 GET。
    onGuarded("/api/diag/freeze", HTTP_GET, [this]() { this->handleAPIDiagFreeze(); });
    onGuarded("/api/diag/freeze", HTTP_DELETE, [this]() { this->handleAPIClearDiagFreeze(); });
    // 崩溃现场（v1.3.50）—— 212 没有串口，崩溃栈只能靠固件自己留（见 crash_record.h）
    onGuarded("/api/diag/crash", HTTP_GET, [this]() { this->handleAPIDiagCrash(); });
    onGuarded("/api/diag/crash", HTTP_DELETE, [this]() { this->handleAPIClearDiagCrash(); });
    
    // 处理OPTIONS请求（CORS预检）
    server.onNotFound([this]() {
        if (server.method() == HTTP_OPTIONS) {
            this->addCORSHeaders();
            server.send(200, "text/plain", "");
        } else {
            server.send(404, "text/plain", "Not Found");
        }
    });
    
    return true;
}

void WebServer::handleClient() {
    // 扫描兜底：前端点了"搜索"之后直接关掉页面，就再也没人来取结果了。
    // 不留这道检查，设备会一直停在"热点+站点"双模上（见 WIFI_SCAN_TIMEOUT_MS）。
    // 注意这里只判超时、不在扫描进行中切模式：切模式会打断扫描，而 core 的
    // `_scanStarted` 只有扫描结束回调才清（详见 handleAPIWifiScan 的注释）。
    // 所以 finishWifiScan() 只在"已经确定扫完了"或"确实超时了"这两种情况下调用。
    if (wifiScanRunning && (millis() - wifiScanStartedAt) > WIFI_SCAN_TIMEOUT_MS) {
        Serial.println(F("[配网] 扫描超时（前端未取结果），切回纯热点"));
        finishWifiScan();
    }
    server.handleClient();
}

void WebServer::stop() {
    if (running) {
        server.stop();
        running = false;
        Serial.println(F("Web server stopped"));
    }
}

void WebServer::start() {
    if (!running) {
        server.begin();
        running = true;
        Serial.println(F("Web server started"));
    }
}

bool WebServer::isRunning() const {
    return running;
}

void WebServer::handleRoot() {
    // 发"内联版"：CSS/JS 已内联进 HTML，面板一次请求即可拿全。
    // 为什么不用分开的 /app.css + /app.js：浏览器会并发开 3 条连接，而本机空闲堆
    // 只有约 2.8KB，并发响应会把堆抽干 → WiFi 驱动在系统上下文处理信标时空指针崩溃
    // （Exception 29，wDev_ProcessFiq → memcpy），表现就是"打开/刷新网页把设备刷重启"。
    sendAsset_P(200, WEB_INDEX_BUNDLE_TYPE, WEB_INDEX_BUNDLE, WEB_INDEX_BUNDLE_LEN);
}

// 发送 gzip 压缩的内置资源。
// 用 send_P(code, type, data, len) 而不是 send_P(code, type, data)：
// 后者靠 strlen_P 量长度，遇到压缩数据里的 0x00 会截断。
void WebServer::sendAsset_P(int code, PGM_P contentType, const uint8_t* data, size_t len) {
    // 内置资源在构建时就已 gzip 压缩（tools/gen_web_assets.py），这里原样吐出并声明编码。
    //
    // 故意不去检查请求头里的 Accept-Encoding，两个原因：
    //   1) ESP8266WebServer 默认只收集 Authorization / Content-Type，想读 Accept-Encoding
    //      必须先 collectHeaders()；而两参数版会因为 int->size_t 的隐式转换输给框架那个
    //      可变参数模板重载，那个版本在本工程编译标准下编译不过 —— 是个白踩的坑。
    //   2) 现代浏览器（含国内各种套壳浏览器）无一例外都声明支持 gzip，检查它只有副作用：
    //      一旦头没被收集到，判定恒为 false，面板会被整体拒成 406 打不开。
    // 命令行自测请用 `curl --compressed`（裸 curl 不发 Accept-Encoding，拿到的是压缩字节）。
    server.sendHeader("Content-Encoding", "gzip");
    // 网页与固件同号更新，别让浏览器拿旧缓存
    server.sendHeader("Cache-Control", "no-cache");
    // 必须用带长度的 send_P：压缩数据里含 0x00，依赖 strlen_P 量长度会被截断
    server.send_P(code, contentType, (PGM_P)data, len);
}

void WebServer::handleSetupPage() {
    // 配网页与面板走同一条"内置资源 + gzip"通道（改动原因见 web/setup.html 头部注释）
    sendAsset_P(200, WEB_SETUP_HTML_TYPE, WEB_SETUP_HTML, WEB_SETUP_HTML_LEN);
}

// 把用户可控的文本（如 WiFi 名称）安全地放进 HTML。
// 必须做：扫描到的 SSID 是"别人广播什么就是什么"，原样拼进页面等于把注入权
// 交给隔壁的恶意热点（虽然只是设备自己的页面，但没有理由留这个口子）。
static String escapeHtmlText(const String& in) {
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        const char c = in.charAt(i);
        switch (c) {
            case '<':  out += F("&lt;");   break;
            case '>':  out += F("&gt;");   break;
            case '&':  out += F("&amp;");  break;
            case '"':  out += F("&quot;"); break;
            case '\'': out += F("&#39;");  break;
            default:   out += c;
        }
    }
    return out;
}

// 保存 / 回退之后的收尾页。
// 这一页只活 3 秒（随后设备重启），所以不引整套主题资源，只把用到的几个变量
// 按 web/theme.css 的值写在这里 —— 少一次请求，也少一份 RAM 峰值。
// ok=false 时不带重启倒计时，而是给一条返回配置页的出路。
void WebServer::sendOutcomePage(bool ok, const String& heading, const String& detail) {
    String html;
    html.reserve(1600);
    html = F("<!DOCTYPE html><html lang='zh-CN'><head><meta charset='UTF-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'>"
             "<title>");
    html += escapeHtmlText(heading);
    html += F("</title><style>"
              ":root{--bg:#f5f5f5;--surface:#fff;--text:#333;--text-3:#666;"
              "--ok:#2e7d32;--bad:#c62828;--track:#e0e0e0;--bar:#2e7d32}"
              "@media(prefers-color-scheme:dark){:root{--bg:#14171c;--surface:#1d2128;--text:#e6e9ee;"
              "--text-3:#a9b0ba;--ok:#7cc47f;--bad:#ff6f6b;--track:#343b44;--bar:#0a7ea4}}"
              "body{margin:0;box-sizing:border-box;padding:24px;min-height:100vh;display:flex;"
              "align-items:center;justify-content:center;background:var(--bg);color:var(--text);"
              "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Arial,sans-serif}"
              ".box{box-sizing:border-box;width:100%;max-width:420px;padding:24px;text-align:center;"
              "background:var(--surface);border-radius:8px}"
              ".mark{font-size:36px;line-height:1}"
              "h1{font-size:1.125rem;margin:10px 0 12px}"
              "p{margin:0 0 8px;font-size:.9rem;line-height:1.5;color:var(--text-3)}"
              "p b,p strong{color:var(--text)}"
              "a{color:#0a7ea4}"
              ".track{height:6px;border-radius:3px;background:var(--track);overflow:hidden;margin-top:16px}"
              ".fill{height:100%;width:0;background:var(--bar);animation:go 3s linear forwards}"
              "@keyframes go{from{width:0}to{width:100%}}"
              "</style></head><body><div class='box'>");

    html += ok ? F("<div class='mark' style='color:var(--ok)'>&#10003;</div>")
               : F("<div class='mark' style='color:var(--bad)'>&#10007;</div>");
    html += F("<h1>");
    html += escapeHtmlText(heading);
    html += F("</h1><p>");
    html += detail;                       // 调用方已转义需要转义的部分
    html += F("</p>");

    if (ok) {
        html += F("<p>设备将在 <b>3</b> 秒后重启。</p>"
                  "<div class='track'><div class='fill'></div></div>");
    } else {
        html += F("<p><a href='/setup'>返回配置页</a></p>");
    }
    html += F("</div></body></html>");

    server.send(200, F("text/html; charset=utf-8"), html);
}

// 保存配网表单（/save）。
//
// ⚠️ 这个接口**同时写 WiFi 和 MQTT**——2026-09-26 正是栽在这里：为验证 MQTT 通路走了
//    /save，却用整片备份里过期的空密码回填，结果把 WiFi 顶掉、设备退进 AP 模式。
//    现在整页统一一条规则来兜住这类误操作：
//      **输入框留空 = 这一项保持原样**（唯一例外见下面两处）
//      · WiFi 密码留空 + SSID 没变 ⇒ 沿用已保存的密码
//      · WiFi 密码留空 + SSID 变了 ⇒ 视为新网络没有密码（开放网络），页面上会先确认一次
//      · MQTT 服务器留空 ⇒ 不动 MQTT；要彻底关掉有专门的勾选框 mqtt_clear
//      · MQTT 用户名 / 密码 / 端口留空 ⇒ 沿用已保存的值
//    为什么这么定：设备可能吊在天花板上，配错一次就是一整趟返工，而"留空"在手机上
//    容易被理解为"我不改这一项"。宁可用户多填一个字，也不要静默清掉他的配置。
void WebServer::handleSave() {
    String ssid = server.arg("ssid");
    ssid.trim();
    String password = server.arg("password");          // 密码不做 trim：空格可能是密码的一部分
    String mqtt_server = server.arg("mqtt_server");
    mqtt_server.trim();
    String mqtt_port = server.arg("mqtt_port");
    String mqtt_username = server.arg("mqtt_username");
    mqtt_username.trim();
    String mqtt_password = server.arg("mqtt_password");

    NetworkConfig& current = configManager.getConfig();
    const bool sameWifi = (ssid == String(current.wifi_ssid));
    const bool sameMqttServer = (mqtt_server == String(current.mqtt_server));
    const bool keepWifiPassword = (password.length() == 0 && sameWifi);
    const bool clearMqtt = server.hasArg("mqtt_clear");

    Serial.printf_P(PSTR("[配网] 收到提交: ssid长度=%u, MQTT=%s, 沿用WiFi密码=%s\n"),
                    ssid.length(),
                    clearMqtt ? "关闭" : (mqtt_server.length() ? "更新" : "不改动"),
                    keepWifiPassword ? "是" : "否");

    // 空 ssid 直接报错退回，绝不存成"无配置"再重启（否则会陷入回 AP 的死循环）
    if (ssid.length() == 0) {
        Serial.println(F("[配网] 错误: ssid 为空，放弃保存"));
        sendOutcomePage(false, F("没有保存"),
                        F("WiFi 名称不能为空。请返回上一页填写后再保存。"));
        return;
    }

    if (keepWifiPassword) {
        password = String(current.wifi_password);
    }

    // 入库前的长度/合法性校验，与串口 AT 走**同一个**函数。
    // 字段是定长 char[]，strncpy 会静默截断：以前密码多敲一位照样"保存成功"，
    // 然后设备连不上、退回热点模式，用户还得跑一趟现场去救。
    String cfgErr = NetworkConfigManager::checkWiFi(ssid, password, !keepWifiPassword);
    if (cfgErr.length()) {
        Serial.print(F("[配网] 校验失败: "));
        Serial.println(cfgErr);
        sendOutcomePage(false, F("没有保存"), cfgErr + F("<br>请返回上一页修改后再保存。"));
        return;
    }
    configManager.setWiFi(ssid, password);

    if (clearMqtt) {
        configManager.clearMQTT();
    } else if (mqtt_server.length() > 0) {
        uint16_t port = mqtt_port.toInt();
        if (port == 0) {
            // 表单规则：端口留空 = 沿用已保存的值（换服务器则回到 1883 默认端口）。
            // 注意这跟"用户手打了 0 或乱码"分不开 —— 所以下面 checkMQTT 再兜一层，
            // 任何入口最后都必须过那道闸。
            port = sameMqttServer ? current.mqtt_port : 1883;
        }
        if (mqtt_username.length() == 0 && sameMqttServer) {
            mqtt_username = String(current.mqtt_username);
        }
        if (mqtt_password.length() == 0 && sameMqttServer) {
            mqtt_password = String(current.mqtt_password);
        }
        cfgErr = NetworkConfigManager::checkMQTT(mqtt_server, port, mqtt_username, mqtt_password);
        if (cfgErr.length()) {
            Serial.print(F("[配网] MQTT 校验失败: "));
            Serial.println(cfgErr);
            sendOutcomePage(false, F("没有保存"), cfgErr + F("<br>请返回上一页修改后再保存。"));
            return;
        }
        configManager.setMQTT(mqtt_server, port, mqtt_username, mqtt_password);
    }

    bool saved = configManager.save();
    Serial.printf_P(PSTR("[配网] 配置写入%s\n"), saved ? "成功" : "失败");

    if (!saved) {
        sendOutcomePage(false, F("保存失败"),
                        F("配置文件写入出错。请重试；若反复失败，可用串口 AT+RESTORE 回退。"));
        return;
    }

    // 硬件确认：存成功后 LED 快闪 3 下，即使浏览器没收到页面，你也能确认设备收到了提交
    for (int i = 0; i < 3; i++) {
        ledControl.setState(LED_ON);
        delay(120);
        ledControl.setState(LED_OFF);
        delay(120);
    }

    // 说清"接下来该干什么"：设备会重启并去连新网络，而用户手机此刻多半还连在设备热点上
    String detail = F("设备正在重启并连接 <strong>");
    detail += escapeHtmlText(ssid);
    detail += F("</strong>。手机请把 Wi-Fi 切到同一个网络，再回到设备地址打开面板"
                "（不确定新地址就查路由器的客户端列表）。");

    sendOutcomePage(true, F("配置已保存"), detail);

    // 关键修复：ESP8266 的 server.send() 只是把响应塞进 lwIP 缓冲，
    // 必须留出时间真正发到浏览器，否则紧跟其后的 ESP.restart() 会让 AP 掉电、
    // 浏览器连半截页面都收不到，表现为"提交后毫无反应"。
    // 这里在 3 秒倒计时内持续 handleClient() + yield，确保响应完整送达。
    for (int i = 0; i < 30; i++) {
        server.handleClient();
        yield();
        delay(100);
    }
    Serial.println(F("[配网] 配置已保存，准备重启"));
    ESP.restart();
}

// ========== HTTP API 处理方法 ==========

void WebServer::addCORSHeaders() {
    // ⚠️ 以前这里是 `Access-Control-Allow-Origin: *`。配合"全站无鉴权"，
    // 意味着**任意网页**都能 fetch('http://<设备IP>/api/maps', {method:'PUT'})
    // 改规则 / 删规则 / 发红外，而且响应可读（还能拿来做内网探测）。
    // 受害者只要打开一个恶意页面，浏览器就是跳板，绕过所有网络隔离。
    //
    // 面板是设备自己发的（同源），本来就不需要 CORS。这里把允许的来源收敛成
    // "请求自己的 Host" ⇒ 等价于只放行同源，跨站一律被浏览器拦掉
    // （顺带挡住 DNS rebinding：伪造域名过来的请求 Host 不在白名单里）。
    // hostHeader() 是本框架无条件解析的（不受 collectHeaders 限制），可直接读。
    const String& host = server.hostHeader();
#ifdef WEB_ALLOW_ANY_HOST
    server.sendHeader("Access-Control-Allow-Origin", "*");   // 逃生开关，见 hostAllowed()
#else
    if (host.length() > 0 && hostAllowed(host)) {
        server.sendHeader("Access-Control-Allow-Origin", "http://" + host);
        server.sendHeader("Vary", "Origin");
    }
#endif
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

bool WebServer::hostAllowed(const String& host) {
#ifdef WEB_ALLOW_ANY_HOST
    // 逃生开关：确有需要"从别的机器/别的端口打开页面再调 API"（例如本地起个
    // 静态服务预览 web/ 目录）时，在 platformio.ini 加 -D WEB_ALLOW_ANY_HOST=1。
    // **默认关闭** —— 开着等于放弃 Host 白名单与同源 CORS 这两道防线。
    (void)host;
    return true;
#else
    // 裸 host（已去掉端口）。白名单 = 设备自己的两个地址，别的都不认。
    String h = host;
    int colon = h.indexOf(':');
    if (colon > 0) {
        h = h.substring(0, colon);
    }
    if (h.length() == 0) return false;
    if (h == "192.168.4.1") return true;                 // AP 配网地址（固定）
    if (h == WiFi.softAPIP().toString()) return true;    // AP 实际地址
    if (h == WiFi.localIP().toString()) return true;     // STA 地址
    return false;
#endif
}

void WebServer::sendJSONResponse(int code, const String& json) {
    addCORSHeaders();
    // ⚠️ v1.3.53：**任何**超过 ~512 B 的响应都必须分块写，不能一次性 send()。
    //
    //   框架里 `HTTP_MAX_SEND_WAIT = 5000  // ms to wait for data chunk to be ACKed`
    //   （ESP8266WebServer.h:64）：`_currentClient.write(buf, len)` 一次最多等 5 s，
    //   超时只打一行 DBGWS 就**放弃剩余内容**。而 `_prepareHeader` 已经把响应头
    //   **单独**写出去了 ⇒ 客户端看到的就是「200 + Content-Length + body 0 字节」。
    //
    //   实测（2026-10-05，111 + MQTT + 环境红外）：设备连续运行一段后，**无线收发会
    //   周期性发呆**（ping 丢包 0% → 40%、RTT 抖动到 747 ms，而网关始终 0~1 ms，
    //   RSSI 仍 -44 dBm）。此时 `/api/status`（一次性写 816 B）必现
    //   `200 / 7.5 s / 0 字节`；而 `/api/maps`、`/api/devices`（512 B 分块）能完整
    //   发出去（devices 慢到 9.9 s 但 1765 B 一个不少）。
    //   ⇒ 分块把"整块丢掉"变成"逐块慢传"，空 body 消失。
    //   单块取 512 B：与既有 maps/devices 分块路径一致，也远小于 5 s 窗口能承载的量。
    const size_t CHUNK_THRESHOLD = 512;
    const size_t CHUNK_SIZE = 512;
    // 关 Nagle：小响应若被攒着等 ACK，会白加一个 RTT（实测地板 ~150 ms，见 TASKS.md 第 53 轮）
    server.client().setNoDelay(true);
    if (json.length() > CHUNK_THRESHOLD && server.chunkedResponseModeStart(code, "application/json")) {
        for (size_t off = 0; off < json.length(); off += CHUNK_SIZE) {
            size_t n = (json.length() - off) > CHUNK_SIZE ? CHUNK_SIZE : (json.length() - off);
            server.sendContent(json.c_str() + off, n);
            yield();
        }
        server.chunkedResponseFinalize();
    } else {
        server.send(code, "application/json", json);
    }
}

void WebServer::sendErrorResponse(int code, const String& error) {
    DynamicJsonDocument doc(256);
    doc["success"] = false;
    doc["error"] = error;
    String output;
    output.reserve(256);
    serializeJson(doc, output);
    sendJSONResponse(code, output);
}

// APIResponseHandler 接口实现
bool WebServer::sendSuccess(const String& json, const char* context) {
    sendJSONResponse(200, json);
    return true;
}

bool WebServer::sendError(const String& error, int code, const char* context) {
    // Web API 使用 HTTP 状态码，code 参数作为 HTTP 状态码
    if (code == 0) code = 500;  // 默认 500
    sendErrorResponse(code, error);
    return false;
}

// 内存检查辅助函数：在API处理前检查内存是否充足
// requiredBytes: 所需的最小连续内存块大小（字节）
// 返回true表示内存充足，false表示内存不足
bool WebServer::checkMemoryBeforeAPI(size_t requiredBytes) {
    return MemoryMonitor::checkMemory(requiredBytes);
}

void WebServer::handleAPIStatus() {
    // 简单API，使用较小的内存检查阈值
    if (!checkMemoryBeforeAPI(256)) {
        sendErrorResponse(503, "Insufficient memory");
        return;
    }
    
    if (mapper == nullptr || forwarder == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }

    // 真实联网状态（不再硬编码 "running"，配网排障直接看这里）
    bool apMode = (WiFi.getMode() & WIFI_AP) != 0;
    bool staConnected = (WiFi.status() == WL_CONNECTED);
    bool configured = configManager.isValid();
    bool mqttConfigured = configManager.isMQTTConfigured();
    bool mqttConnected = (mqtt != nullptr) && mqtt->isConnected();
    
    // 使用统一格式
    DynamicJsonDocument doc(1536);
    JsonObject dataObj = doc.createNestedObject("data");
    dataObj["status"] = apMode ? "ap" : (staConnected ? "running" : "offline");
    dataObj["ruleCount"] = mapper->getRuleCount();
    // 转发总开关（持久化，与 MQTT/HA 的 data.forwardingEnabled 同一语义）
    dataObj["forwardingEnabled"] = forwarder->isEnabled();
    // 此刻是否正在发射红外 —— 和上面那个"总开关"是两回事，别混用
    dataObj["transmitting"] = forwarder->isForwarding();
    // 固件身份：OTA 之后用它确认设备上跑的到底是哪一版
    dataObj["firmware"] = FW_VERSION;
    dataObj["sketchMD5"] = ESP.getSketchMD5();
    dataObj["ota"] = true;
    dataObj["otaPort"] = OTA_PORT;
    dataObj["uptime"] = (unsigned long)(millis() / 1000);
    dataObj["freeHeap"] = MemoryMonitor::getFreeHeap();
    dataObj["heapUsage"] = MemoryMonitor::getUsagePercent();
    // 最大连续块 / 碎片率：判断"面板为什么被内存兜底停掉"的关键指标，
    // 单看 freeHeap 会漏掉"总量够但拿不出一块连续内存"的情况
    dataObj["maxFreeBlock"] = MemoryMonitor::getLargestFreeBlock();
    dataObj["fragmentation"] = MemoryMonitor::getFragmentationPercent();

    // 上次为什么重启（v1.3.9）：用来区分"被压崩了"和"自己主动重启 / OTA 重启"。
    // resetReason=2 (exception) 就是崩溃；4 (software-restart) 是 OTA 或主动重启。
    dataObj["resetReason"] = HealthGuard::getResetReason();
    dataObj["resetReasonText"] = String(HealthGuard::getResetReasonText());
    dataObj["lowMemRestarts"] = HealthGuard::getLowMemRestartCount();

    // 网络僵死自愈（v1.3.36）：三个字段合起来，远程就能回答
    // "这台设备最近有没有僵死过、这次启动是不是被它救回来的、现场还在不在"。
    dataObj["netRestartCount"] = NetWatchdog::getRestartCount();
    dataObj["netRestartedByNetwork"] = NetWatchdog::didRestartByNetwork();
    dataObj["hasFreezeSnapshot"] = NetWatchdog::hasSnapshot();
    // 崩溃自留档（v1.3.50）：212 无串口 ⇒ 这是"上次是不是崩过、现场还在不在"的**唯一**线索。
    // 监控脚本靠它发现新崩溃（见 tools/crash_watch.py）。现场内容在 /api/diag/crash。
    dataObj["hasCrashRecord"] = CrashRecord::has();

    // 安全模式的"可疑启动计数"（v1.3.15）：0 表示这一版固件启动干净；
    // 累加到 bootFailLimit 就会自动进安全模式。封壳后没有串口、也看不到启动横幅，
    // 这两个字段是唯一能远程确认"设备离安全模式还有多远"的窗口。
    dataObj["bootFailCount"] = SafeMode::getBootFailCount();
    dataObj["bootFailLimit"] = SAFE_MODE_MAX_FAILS;

    // 联网诊断（配网排障核心字段）
    dataObj["wifiConfigured"] = configured;                       // 配置文件里有没有 ssid
    dataObj["wifiMode"] = apMode ? "AP" : (staConnected ? "STA" : "NONE");
    dataObj["wifiConnected"] = staConnected;                     // 当前是否真的连上 WiFi
    dataObj["wifiSSID"] = configured ? String(configManager.getConfig().wifi_ssid) : "";
    dataObj["localIP"] = staConnected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    if (staConnected) {
        dataObj["rssi"] = WiFi.RSSI();
    }

    // MQTT（配了才显示服务器，没配就是空）
    dataObj["mqttConfigured"] = mqttConfigured;
    dataObj["mqttConnected"] = mqttConnected;
    dataObj["mqttServer"] = mqttConfigured ? String(configManager.getConfig().mqtt_server) : "";
    // 配网页要预填这两项，否则"只改服务器"会把用户名一起清掉（与 WiFi 密码同类陷阱）
    dataObj["mqttPort"] = configManager.getConfig().mqtt_port;
    dataObj["mqttUsername"] = mqttConfigured ? String(configManager.getConfig().mqtt_username) : "";
    // 只报"有没有存过密码"，不报密码本身：
    // 配网页据此把提示写成"留空 = 不修改"还是"首次配网请填密码"。
    dataObj["wifiPasswordSet"] = (strlen(configManager.getConfig().wifi_password) > 0);
    dataObj["mqttPasswordSet"] = (strlen(configManager.getConfig().mqtt_password) > 0);
    
    // ⭐ v1.3.52：**不要**再把 dataObj 交给 APIResponse::success()。
    //   那条路会把这 30 个字段**再深拷进第二个 doc**，而包装层对 JsonObject 只估
    //   256+512=768 ⇒ 必然溢出 ⇒ 再开 largerDoc(1536) 深拷一遍 ⇒ `doc = largerDoc`
    //   又是拷贝赋值（copyAssignFrom → reallocPool，**再要一份等大池**）
    //   ⇒ 一次 /api/status 的瞬时峰值凭空多 ~3.8 KB，而带 MQTT 的空闲堆只有 ~5.6 KB。
    //   实测后果：heap 稍低（环境有红外在打）时整条**响应体写不出去**，客户端拿到
    //   `200 + 空 body`（框架 HTTP_MAX_SEND_WAIT=5000 卡满 5 s 才放弃，所以耗时 ~7.4 s）；
    //   同一毫秒 /api/maps 照常秒回 —— 因为 maps 走的是 processJsonArrayResponse
    //   的**字符串直拼快路**（零解析、零第二 doc）。这里就是把 status 也拉回这条路。
    //   现在峰值 = doc(1536) + output(≈1 KB)，**只留一份 doc**。
    String output;
    output.reserve(measureJson(dataObj) + 160);   // 外壳 ~60 B + 余量
    output = F("{\"success\":true,\"status\":\"success\",\"data\":");
    serializeJson(dataObj, output);               // 字符串转义照旧由库负责，字段/顺序不变
    output.concat(F(",\"timestamp\":"));
    output += String(millis());
    output += '}';
    sendJSONResponse(200, output);
}

// 转发总开关：PUT/POST /api/forwarding  body: {"enabled":true}
// 内存与落盘同时生效；关掉后 IRForwarder::forwardSignal 直接返回 false。
void WebServer::handleAPISetForwarding() {
    if (forwarder == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }

    String body = server.arg("plain");
    body.trim();
    if (body.length() == 0) {
        sendErrorResponse(400, "Missing request body");
        return;
    }

    DynamicJsonDocument doc(192);
    DeserializationError err = deserializeJson(doc, body);
    if (err || !doc.containsKey("enabled")) {
        sendErrorResponse(400, "Body must be {\"enabled\":true|false}");
        return;
    }

    bool enabled = doc["enabled"].as<bool>();
    forwarder->setEnabled(enabled);
    DeviceState::setForwardingEnabled(enabled);

    DynamicJsonDocument resp(320);
    JsonObject dataObj = resp.createNestedObject("data");
    dataObj["forwardingEnabled"] = enabled;
    sendJSONResponse(200, APIResponse::success(dataObj, logger));
}

void WebServer::handleAPIMaps() {
    if (mapper == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 优先流式读取 LittleFS 文件并直接发送，避免整份数据进内存
    if (LittleFS.exists(CONFIG_FILE_MAPS)) {
        File file = LittleFS.open(CONFIG_FILE_MAPS, "r");
        if (file) {
            size_t fileSize = file.size();
            if (fileSize > 0 && fileSize < 32768) {
                addCORSHeaders();
                if (server.chunkedResponseModeStart(200, "application/json")) {
                    server.sendContent(F("{\"success\":true,\"status\":\"success\",\"data\":"));
                    const size_t BUF = 512;
                    char buf[BUF];
                    while (file.available()) {
                        size_t n = file.read((uint8_t*)buf, BUF);
                        if (n > 0) server.sendContent(buf, n);
                        yield();
                    }
                    file.close();
                    server.sendContent(F(",\"timestamp\":"));
                    server.sendContent(String(millis()));
                    server.sendContent(F("}"));
                    server.chunkedResponseFinalize();
                    return;
                }
            }
            file.close();
        }
    }
    
    // 回退：使用公共函数构建 data 再包装响应
    String dataJson = APIHandlers::handleQueryMaps(*mapper, logger);
    processJsonArrayResponse(dataJson, "/api/maps", 16384);
}

void WebServer::handleAPIUpdateMap() {
    String body = server.arg("plain");
    if (body.length() == 0) {
        sendErrorResponse(400, "Missing request body");
        return;
    }
    
    if (mapper == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 使用公共函数
    bool success = false;
    String message = APIHandlers::handleUpdateMap(*mapper, body, success);
    
    if (success) {
        sendJSONResponse(200, APIResponse::success(message, logger));
    } else {
        sendErrorResponse(400, message);
    }
}

void WebServer::handleAPILogs() {
    if (logger == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 使用公共函数（返回全部日志，环形缓冲最多 MAX_LOG_ENTRIES = 10 条）
    String dataJson = APIHandlers::handleGetLogs(*logger);
    
    // 使用统一的响应处理
    processJsonArrayResponse(dataJson, "/api/logs", 4096);
}

void WebServer::handleAPILearnStart() {
    String body = server.arg("plain");
    if (body.length() == 0) {
        sendErrorResponse(400, "Missing request body");
        return;
    }
    
    if (learner == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 使用公共函数
    bool success = false;
    String message = APIHandlers::handleStartLearn(*learner, body, success);
    
    if (success) {
        sendJSONResponse(200, APIResponse::success(message, logger));
    } else {
        sendErrorResponse(400, message);
    }
}

void WebServer::handleAPILearnStatus() {
    if (learner == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 使用公共函数（简化版）
    String dataJson = APIHandlers::handleQueryLearnStatus(*learner);
    
    // 使用统一的响应处理
    processJsonObjectResponse(dataJson, "/api/learn/status", 1024);
}

void WebServer::handleAPILearnStop() {
    if (learner == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 使用公共函数
    bool success = false;
    String message = APIHandlers::handleStopLearn(*learner, success);
    
    if (success) {
        sendJSONResponse(200, APIResponse::success(message, logger));
    } else {
        sendErrorResponse(500, message);
    }
}

void WebServer::handleAPIClearLearnResult() {
    if (learner == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    if (learner->isLearningDevice()) {
        // 学习中进行中清除会把刚学到的抹掉，而界面还在显示进度 ⇒ 直接拒。
        sendErrorResponse(409, "Learning in progress");
        return;
    }
    const bool removed = learner->clearPersistedResult();
    sendJSONResponse(200, APIResponse::success(
        removed ? String("learned result cleared") : String("nothing to clear")));
}

void WebServer::handleAPILearnResult() {
    if (learner == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 获取格式参数（默认base64）
    String format = server.arg("format");
    if (format.length() == 0) {
        format = "base64";  // 默认格式
    }
    
    // 使用公共函数
    APIHandlers::LearnQueryStatus st = APIHandlers::LearnQueryStatus::OK;
    String dataJson = APIHandlers::handleQueryLearnResult(*learner, format, &st);
    if (dataJson.length() == 0) {
        // 三种失败分别给不同的状态码与说明。以前一律 404 "没有学习结果"，
        // 于是"设备只是当时内存不够"会被报成"你压根没学过" ⇒ 用户重学一遍
        // 还是同样的结果，属于不可自愈的假诊断。
        switch (st) {
            case APIHandlers::LearnQueryStatus::NO_DATA:
                sendErrorResponse(404, "No learned device found");
                break;
            case APIHandlers::LearnQueryStatus::LOW_MEMORY:
                sendErrorResponse(503, "Insufficient memory");
                break;
            default:
                sendErrorResponse(500, "Failed to convert learned result");
                break;
        }
        return;
    }
    
    // ⚠️⚠️ 这里原来还有一块 `DynamicJsonDocument dataDoc(4096)` + deserializeJson(dataJson)，
    //   只为了判断"是对象还是数组"、再把内容**内联**进 data。三个问题：
    //   ① 白要 4 KB **连续**内存 —— 而"刚学完"正是内存最低的时刻（学习有一次性台阶，
    //      实测 maxBlock 从 ~11 KB 落到 2792 B）⇒ 这块必然分配失败；
    //   ② ArduinoJson 分配失败后 deserializeJson 返回 NoMemory，代码走 `err` 分支
    //      ⇒ data 退回**字符串**。于是同一个接口**时而给对象、时而给字符串**，
    //      前端按对象写的取法（obj.commands）在内存紧张时恒 undefined
    //      ⇒ 用户报的"学习完成了但不显示学习结果"；
    //   ③ dataJson 本身就是合法 SmartIR JSON 文本，作为字符串放进 data 完全够用
    //      （前端 JSON.parse 即可），内联那一步没有任何收益。
    //   现在统一按字符串返回 —— 响应体随内容定长，行为不再随内存摆动。
    String output = APIResponse::success(dataJson, logger);
    sendJSONResponse(200, output);
}

void WebServer::handleAPIRecentSignals() {
    if (logger == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // since=<毫秒时间戳>：只回更新的条目（前端实时监视用增量拉取，无新信号时响应极小）
    unsigned long since = 0;
    if (server.hasArg("since")) {
        since = strtoul(server.arg("since").c_str(), nullptr, 10);
    }
    
    // 使用公共函数
    String dataJson = APIHandlers::handleGetRecentSignals(*logger, since);
    
    // 使用统一的响应处理
    processJsonArrayResponse(dataJson, "/api/signals/recent", 2048);
}

void WebServer::handleAPIClearRecentSignals() {
    if (logger == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }

    // 复用 Logger::clearRecentSequences()（logger.cpp）—— 它本来就是为"释堆"写的：
    // recentSequences 里的 timingBase64 是整段堆分配（200 时序点 ≈ 534 字符），
    // 是本系统最大的一块可回收堆。所以"用户点清空"和"堆见底软恢复"
    // 走的是同一个函数，行为一致，不需要第二份实现。
    //
    // ⚠️ 清完是**真的没了**，没有 undo：清空后按遥控器，新信号会重新出现。
    // 前端清完必须把本地列表也清掉（见 web/app.js 的 clearRecentSignals）——
    // 否则设备端空、浏览器还留着旧条目，看起来像"没清掉"。
    logger->clearRecentSequences();
    sendJSONResponse(200, APIResponse::success(String("recent signals cleared")));
}

void WebServer::handleAPISendSignal() {
    String body = server.arg("plain");
    if (body.length() == 0) {
        sendErrorResponse(400, "Missing request body");
        return;
    }
    
    if (forwarder == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    
    // 使用公共函数
    bool success = false;
    String message = APIHandlers::handleSendSignal(*forwarder, body, success);
    
    if (success) {
        sendJSONResponse(200, APIResponse::executing(message));
    } else {
        sendErrorResponse(500, message);
    }
}

// 转发记录查询：最近若干次"收到 A → 匹配 → 发射"的结论
void WebServer::handleAPIGetForwardRecords() {
    if (forwarder == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    String dataJson = APIHandlers::handleGetForwardRecords(*forwarder);
    processJsonArrayResponse(dataJson, "/api/forward/records", 2048);
}

void WebServer::handleAPIClearForwardRecords() {
    if (forwarder == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }
    forwarder->clearForwardRecords();
    sendJSONResponse(200, APIResponse::success(String("records cleared")));
}

// ── 设备 / 按钮配置（v1.3.10）─────────────────────────────────────────────
// 以前这份配置只活在浏览器的 localStorage 里：换一台手机、或清一次缓存就全没了，
// 而界面上还写着"已保存到设备"——名不副实。现在真正存到设备的 /devices.json。
//
// 实现上刻意**不解析 JSON**：设备空闲堆只有 12 KB 上下，而一份设备配置可能有几 KB，
// 若走 DynamicJsonDocument 解析再序列化，峰值要翻两三倍，很容易把堆抽干。
// 这里只做"原文存取"，内存峰值 ≈ 2 × 文件大小，由 MAX_DEVICES_FILE_SIZE 兜住。
void WebServer::handleAPIGetDevices() {
    // 手工拼响应，保持与 APIResponse::success 一致的字段（success/status/data/timestamp）
    // ⚠️ timestamp 用**毫秒**（= millis()），与其余所有接口一致。
    // 这里原来写的是 millis()/1000（秒），是全项目唯一一处秒制 —— 同一个字段名
    // 差 1000 倍，前端/脚本按毫秒解析就会算出 1970 年。
    //
    // ⭐ v1.3.42：改成**流式发送**（与 handleAPIMaps 同一套写法）。原来把整文件读进
    //   String、再 reserve 一份 payload 拼起来（峰值约 2×文件大小）—— 配置一到几 KB，
    //   空闲堆就被抽干，**读**自己先失败。现在只用一块 512 B 的栈缓冲，RAM 占用与
    //   配置总大小**无关**。
    //   ⇒ 配合分片写（handleAPISaveDevicesChunk），读写两端都不再受内存约束，
    //     MAX_DEVICES_FILE_SIZE 才敢放到 8192。
    if (server.chunkedResponseModeStart(200, "application/json")) {
        server.sendContent(F("{\"success\":true,\"status\":\"success\",\"data\":"));
        File f = LittleFS.open(CONFIG_FILE_DEVICES, "r");
        if (f && f.size() > 0) {
            const size_t BUF = 512;
            char buf[BUF];
            while (f.available()) {
                const size_t n = f.read((uint8_t*)buf, BUF);
                if (n > 0) server.sendContent(buf, n);
                yield();
            }
            f.close();
        } else {
            // 还没存过 / 空文件 ⇒ 回空数组，前端据此判断要不要把本地配置推上来
            server.sendContent(F("[]"));
            if (f) f.close();
        }
        server.sendContent(F(",\"timestamp\":"));
        server.sendContent(String(millis()));
        server.sendContent(F("}"));
        server.chunkedResponseFinalize();
        return;
    }

    // 回退（HTTP/1.0 客户端不支持 chunked）：一次性拼串。小配置没问题。
    String content = Storage::readFile(CONFIG_FILE_DEVICES);
    if (content.length() == 0) {
        content = "[]";
    }
    String payload;
    payload.reserve(content.length() + 64);
    payload = "{\"success\":true,\"status\":\"success\",\"data\":";
    payload += content;
    payload += ",\"timestamp\":";
    payload += String(millis());
    payload += "}";
    server.send(200, "application/json", payload);
}

void WebServer::handleAPISaveDevices() {
    String body = server.arg("plain");
    if (body.length() == 0) {
        sendErrorResponse(400, "Missing request body");
        return;
    }
    // ⛔ v1.3.42 起这条"整份一次 POST"的路径只当小配置的快路径（外部脚本/兼容）。
    //   前端已改用分片（POST /api/devices/chunk）。原因：整份 POST 要框架**一次读完**
    //   body，读不满就丢连接（`Parsing-impl.h:160-169`，连 4xx 都回不了）。
    //   ⚠️ 别把上限放到 MAX_DEVICES_FILE_SIZE（8192）：那个值由分片路径承担，
    //   这条路放到那么大等于"允许了但发不出去"，用户只看到网络错误。
    if (body.length() > DEVICES_SINGLE_SHOT_MAX) {
        sendErrorResponse(413, "配置过大，请使用分片提交（上限 " + String(DEVICES_SINGLE_SHOT_MAX) + " 字节）");
        return;
    }

    // 只认 JSON 数组，别把垃圾写进 flash（不解析，只看首尾字符，几乎不花内存）
    body.trim();
    if (body.charAt(0) != '[' || body.charAt(body.length() - 1) != ']') {
        sendErrorResponse(400, "Expected a JSON array");
        return;
    }

    // 写文件前留足余量（v1.3.41 重定）。
    //
    // ⚠️ 旧写法是 `body.length()*2 + 2048`，理由是"文件本身 + 一份副本 + 响应开销"。
    //   但那份"副本"**走到这一行时早就存在了，而且不止一份**：
    //   `ESP8266WebServer` 收 body 时同时持 `plainBuf` + `arg.value`（`Parsing-impl.h:160-188`），
    //   `server.arg("plain")` 又返回**一份拷贝** ⇒ 这里已经是 2×body 常驻。
    //   再要求 `2×body+2048`，等于按 **4×** 要 ⇒ 空闲堆 8.4 KB 时可存上限被压到 ~1.3 KB
    //   （实测：body 1203 → 200，body 1448 → 507），而一条学习到的时序码信号 Base64 就 ~384 字符。
    //   真正写盘的 `Storage::saveFileAtomic` 只做 LittleFS 临时文件写入 + rename，峰值约 1 KB。
    //   ⚠️ 后果不小：学 2 条时序码后空闲堆只剩 4.5 KB，此时连 857 B 的配置都过不去（差 51 B）。
    // ⇒ 现在按"再要一份 + 写盘与响应开销"算。
    const size_t needed = body.length() + 1536;
    if (MemoryMonitor::getFreeHeap() < needed) {
        sendErrorResponse(507, "设备内存不足，稍后再试");
        return;
    }

    // 原子写：写坏 devices.json 至少要重新配一遍所有按钮。
    if (!Storage::saveFileAtomic(CONFIG_FILE_DEVICES, body)) {
        sendErrorResponse(500, "保存失败");
        return;
    }
    sendJSONResponse(200, APIResponse::success(String("devices saved")));
}

// ── 设备配置：分片提交（v1.3.42）────────────────────────────────────────────
//
// 为什么要有这条：整份配置一次 POST 时，`ESP8266WebServer` 收 body 要**一次读完**
// （`Parsing-impl.h:160-169`），读不满就 `CLIENT_MUST_STOP` —— 失败形态是 **TCP 被丢**，
// 浏览器只看到网络错误，连一个 4xx 都回不了。实测（111，刚学完 2 条时序码、blk 只剩 4528）
// 1677 B 的配置**必然收不下**；而用户"学完 → 一键加入设备"恰恰总是发生在堆最紧的时刻。
// ⛔ 加内存闸救不了：闸在 handler 里，而失败发生在更早的"收 body"阶段，永远慢一步。
//
// 分片后每片只要 ~3×512 的**连续**块，且**设备端 RAM 占用与配置总大小无关**
// —— 这是与"调大阈值"的本质区别。
//
// 协议：`POST /api/devices/chunk?seq=<n>&total=<N>&len=<L>`（body = UTF-8 字节片）
//   · seq=0     ：丢弃上次的半截 tmp 后新建；校验本片首个非空白字节是 '['
//   · seq=N-1   ：追加后校验「文件大小 == len」且末个非空白字节是 ']'，通过则 rename 成 devices.json
//   · 其余      ：追加
//   响应统一 JSON，含 `received`（累计字节），前端据此对账。
//   ⚠️ 必须**串行**发：设备端是追加写，前端只有在上一片 200 后才发下一片。
// ⭐ 为什么靠 seq 而不是"上传会话 ID"：设备端无需保存会话状态 —— `seq=0` 本身就是
//   "开始新一次上传"的信号（reset）。中断留下的 tmp 会在下次 seq=0 时被删掉，
//   不会长期占 flash（也不需要在启动时扫描清理）。
void WebServer::handleAPISaveDevicesChunk() {
    const String seqArg   = server.arg("seq");
    const String totalArg = server.arg("total");
    const String lenArg   = server.arg("len");
    if (seqArg.length() == 0 || totalArg.length() == 0) {
        sendErrorResponse(400, "Missing seq/total");
        return;
    }
    const long seq   = seqArg.toInt();
    const long total = totalArg.toInt();
    if (total <= 0 || seq < 0 || seq >= total) {
        sendErrorResponse(400, "Bad seq/total");
        return;
    }

    String body = server.arg("plain");
    if (body.length() == 0) {
        sendErrorResponse(400, "Missing chunk body");
        return;
    }
    if (body.length() > DEVICES_CHUNK_MAX_BYTES) {
        sendErrorResponse(413, "分片过大");
        return;
    }

    // 内存闸：只覆盖**单片**峰值，与配置总大小无关 —— 这正是分片的意义。
    // ⚠️ 别按整份配置估：`server.arg("plain")` 那一份拷贝此刻已经存在，这里再要的只是
    //   "写盘 + 拼响应"的余量。数值放宽一点没关系（这条路几乎没有 OOM 风险）。
    if (MemoryMonitor::getFreeHeap() < body.length() + 512) {
        sendErrorResponse(507, "设备内存不足，稍后再试");
        return;
    }

    const size_t tmpSize = Storage::fileSize(DEVICES_TMP_FILE);

    if (seq == 0) {
        // 开始新一次上传：清掉上次的半截文件，并做首字符校验（不解析 JSON，几乎不花内存）
        Storage::deleteFile(DEVICES_TMP_FILE);
        String head = body;
        head.trim();
        if (head.charAt(0) != '[') {
            sendErrorResponse(400, "Expected a JSON array");
            return;
        }
    } else if (tmpSize == 0) {
        // 中间片却找不到 tmp ⇒ 上一次上传已经失败/被清掉。让前端从 seq=0 重来，
        // 而不是静默接在半截内容后面（那会写出一个语法上看不出来的坏配置）。
        sendErrorResponse(409, "Upload session lost, restart");
        return;
    }

    // 累计上限（含本片）。单看一片永远不超，必须按"已有 + 本片"判。
    if (tmpSize + body.length() > MAX_DEVICES_FILE_SIZE) {
        Storage::deleteFile(DEVICES_TMP_FILE);
        sendErrorResponse(413, "设备配置过大（上限 " + String(MAX_DEVICES_FILE_SIZE) + " 字节）");
        return;
    }

    size_t nowSize = 0;
    if (!Storage::appendFile(DEVICES_TMP_FILE, (const uint8_t*)body.c_str(), body.length(), nowSize)) {
        Storage::deleteFile(DEVICES_TMP_FILE);   // 写了一半的文件不能用
        sendErrorResponse(500, "写入失败");
        return;
    }

    if (seq == total - 1) {
        // ── 末片：校验后提交 ────────────────────────────────────────────────
        // 尾部字符校验（跳过尾部空白字节，避免"配置末尾恰好带换行"被误拒）
        int k = body.length() - 1;
        while (k >= 0) {
            const char ch = body.charAt(k);
            if (ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') { k--; continue; }
            break;
        }
        if (k < 0 || body.charAt(k) != ']') {
            Storage::deleteFile(DEVICES_TMP_FILE);
            sendErrorResponse(400, "Expected a JSON array");
            return;
        }
        // 总长校验：前端把算好的总字节数放在 len 里 ⇒ 能发现"少发了一片但前端以为发完"
        if (lenArg.length()) {
            const size_t declared = (size_t)lenArg.toInt();
            if (declared != nowSize) {
                Storage::deleteFile(DEVICES_TMP_FILE);
                sendErrorResponse(400, "Length mismatch (" + String((unsigned)nowSize) +
                                       " != " + String((unsigned)declared) + ")");
                return;
            }
        }
        if (!Storage::commitFile(DEVICES_TMP_FILE, CONFIG_FILE_DEVICES)) {
            sendErrorResponse(500, "保存失败");
            return;
        }
        sendJSONResponse(200, APIResponse::success(String("devices saved")));
        return;
    }

    // 中间片：如实回"收到了多少"，前端据此核对
    String payload;
    payload.reserve(96);
    payload = "{\"success\":true,\"status\":\"success\",\"data\":{\"received\":";
    payload += String((unsigned)nowSize);
    payload += "},\"timestamp\":";
    payload += String(millis());
    payload += "}";
    sendJSONResponse(200, payload);
}

// ── 网络僵死现场（v1.3.36）────────────────────────────────────────────
// 设备因为"主循环活着、WiFi 自认连着、但 LAN 上彻底消失"而自愈重启后，
// 重启前那份现场就在这里。以前这种僵死只能靠串口抓，而且必须赶在它死之前
// 就挂上监听（一断电现场就没了）；现在每次僵死自动存档，重启回来直接 GET 取回。
void WebServer::handleAPIDiagFreeze() {
    if (!checkMemoryBeforeAPI(1024)) {
        sendErrorResponse(503, "Insufficient memory");
        return;
    }
    const String snap = NetWatchdog::readSnapshot();   // 没存过则返回 "{}"

    // 手工拼响应：快照本身就是完整 JSON，**直接原样嵌进 data.freeze**，
    // 不走 DynamicJsonDocument 再解析/重序列化一遍 —— 与 /api/devices 同一个理由（省堆）。
    String payload;
    payload.reserve(snap.length() + 192);
    payload = "{\"success\":true,\"status\":\"success\",\"data\":{\"hasSnapshot\":";
    payload += NetWatchdog::hasSnapshot() ? "true" : "false";
    payload += ",\"netRestartCount\":";
    payload += String(NetWatchdog::getRestartCount());
    payload += ",\"restartedByNetwork\":";
    payload += NetWatchdog::didRestartByNetwork() ? "true" : "false";
    payload += ",\"freeze\":";
    payload += snap;
    payload += "},\"timestamp\":";
    payload += String(millis());
    payload += "}";
    sendJSONResponse(200, payload);
}

void WebServer::handleAPIClearDiagFreeze() {
    NetWatchdog::clearSnapshot();
    sendJSONResponse(200, APIResponse::success(String("freeze snapshot cleared")));
}

// 崩溃现场：与 /api/diag/freeze 同一个套路 —— 快照本身就是完整 JSON，
// 原样嵌进 data.record，不走 DynamicJsonDocument 再解析/重序列化（省堆）。
void WebServer::handleAPIDiagCrash() {
    if (!checkMemoryBeforeAPI(1024)) {
        sendErrorResponse(503, "Insufficient memory");
        return;
    }
    const String rec = CrashRecord::readJson();   // 没存过则 "{}"

    String payload;
    payload.reserve(rec.length() + 128);
    payload = "{\"success\":true,\"status\":\"success\",\"data\":{\"hasRecord\":";
    payload += CrashRecord::has() ? "true" : "false";
    payload += ",\"record\":";
    payload += rec;
    payload += "},\"timestamp\":";
    payload += String(millis());
    payload += "}";
    sendJSONResponse(200, payload);
}

void WebServer::handleAPIClearDiagCrash() {
    CrashRecord::clear();
    sendJSONResponse(200, APIResponse::success(String("crash record cleared")));
}

// 规则试跑：立刻匹配并发射，把"为什么没转发"直接说清楚
void WebServer::handleAPIForwardTest() {
    String body = server.arg("plain");
    if (body.length() == 0) {
        sendErrorResponse(400, "Missing request body");
        return;
    }
    if (forwarder == nullptr) {
        sendErrorResponse(500, "Service not initialized");
        return;
    }

    bool success = false;
    String message = APIHandlers::handleForwardTest(*forwarder, body, success);
    if (success) {
        sendJSONResponse(200, APIResponse::executing(message));
    } else {
        sendErrorResponse(500, message);
    }
}

// 搜索周边 WiFi —— 配网页用，替掉"手机上小写/大写/特殊符号手敲 SSID"这件苦差事。
//
// **只在 AP 模式下允许**（也就是"设备正等着配网"这一种状态），两个理由：
//   1. 设备联网正常时，用户要改的多半只是 MQTT，扫描毫无意义；
//   2. 扫描要临时切到"热点 + 站点"双模（红外接收跟着停摆），
//      而设备联网时这个动作一点用都没有 —— 不联网的 STA 状态下 Web 服务本来也不跑
//      （见 main.cpp 的 shouldWebServerRun），所以 AP 模式是唯一需要也唯一可达的场景。
//
// 纯 AP 模式下 SDK 的站点扫描不可用，得先切到双模；扫完立刻切回来，
// 并重申一次热点参数 —— 此刻手机正连在这个热点上，热点没了用户就得重新连，
// 等于把配网打断。
//
// ⚠️ 两段式：本函数只负责"起扫"并立刻返回，结果由 /api/wifi/scan/result 轮询取走。
// 以前是 `WiFi.scanNetworks(false /*sync*/)`：SDK 在内部等 13 个信道扫完才返回，
// 实测 2~4 秒。而这 2~4 秒是在 HTTP 处理函数里花掉的，HTTP 处理函数又是在 loop()
// 里调用的 ⇒ 红外接收/发射、MQTT 心跳、OTA 握手、健康检查全部停摆。
// 异步版核对过 core 源码（ESP8266WiFiScan.cpp:90-97）：`wifi_station_scan()` 提交后
// 只 `esp_yield()` 一下就 return WIFI_SCAN_RUNNING，全程零阻塞。
void WebServer::handleAPIWifiScan() {
    const bool apMode = (WiFi.getMode() & WIFI_AP) != 0;
    if (!apMode) {
        sendErrorResponse(409, F("设备已经联网，不需要搜索周边 WiFi"));
        return;
    }
    if (wifiScanRunning) {
        // 上一次还没扫完：如实告诉前端"还在扫"，别重起一次
        sendJSONResponse(200, F("{\"success\":true,\"data\":{\"status\":\"scanning\"}}"));
        return;
    }
    if (MemoryMonitor::getFreeHeap() < MEMORY_MIN_FOR_WEBSERVER) {
        sendErrorResponse(503, F("设备内存不足，稍后再试"));
        return;
    }

    // 先切双模再起扫。这一段绝不能出现"扫描期间切模式 / WiFi.begin()"的动作，理由：
    // core 里 `_scanStarted` 这个静态标志**只有扫描结束回调 `_scanDone()` 会清**
    // （核对过 ESP8266WiFiScan.cpp:141-148，scanDelete() 只释放结果、不清这个标志）。
    // 扫描正常结束或 SDK 报失败都会走到 `_scanDone()`（它对 `status != OK` 有专门分支），
    // 但若真被打断到回调不来，`_scanStarted` 会永远为真 ⇒ 之后 scanNetworks() 直接返回
    // WIFI_SCAN_RUNNING 什么都不做 ⇒ "搜索"直到重启前永久失效。所以：
    //   · 切回纯热点只在扫完（或超时兜底）之后做，见 finishWifiScan()；
    //   · WiFiManager 的 AP 回探会让位（见 probeStaFromAP 里那道检查）。
    WiFi.mode(WIFI_AP_STA);
    delay(50);

    Serial.println(F("[配网] 起扫周边 WiFi（异步，主循环照常跑）"));
    if (WiFi.scanNetworks(true /*async*/, true /*show hidden*/) == WIFI_SCAN_FAILED) {
        // 起扫就失败了（SDK 拒绝）—— 别把自己留在双模上
        WiFi.mode(WIFI_AP);
        WiFiManager::configureAP();
        sendErrorResponse(503, F("扫描启动失败，稍后再试"));
        return;
    }
    wifiScanRunning = true;
    wifiScanStartedAt = millis();
    sendJSONResponse(200, F("{\"success\":true,\"data\":{\"status\":\"scanning\"}}"));
}

// 取扫描结果。前端在 /api/wifi/scan 之后每 500ms 调一次，直到拿到 done/failed。
void WebServer::handleAPIWifiScanResult() {
    const int st = WiFi.scanComplete();

    if (st == WIFI_SCAN_RUNNING) {
        // 还在扫。这里也顺手做超时兜底 —— 前端可能已经不问了，但设备不能永远留在双模
        if (wifiScanRunning && (millis() - wifiScanStartedAt) > WIFI_SCAN_TIMEOUT_MS) {
            Serial.println(F("[配网] 扫描超时，切回纯热点"));
            finishWifiScan();
            sendJSONResponse(200, F("{\"success\":true,\"data\":{\"status\":\"failed\"}}"));
            return;
        }
        sendJSONResponse(200, F("{\"success\":true,\"data\":{\"status\":\"scanning\"}}"));
        return;
    }

    if (st == WIFI_SCAN_FAILED) {
        // -2 = "没在扫"（没起过 / 上一次结果已释放 / 扫描中途被打断）。
        // 这里必须清掉 wifiScanRunning，否则上面那个超时兜底会把一次正常的
        // "我还没点过搜索"误判成"扫描卡住了"。
        if (wifiScanRunning) {
            finishWifiScan();
        }
        sendJSONResponse(200, F("{\"success\":true,\"data\":{\"status\":\"idle\"}}"));
        return;
    }

    // st >= 0：扫完了，组装结果
    if (MemoryMonitor::getFreeHeap() < MEMORY_MIN_FOR_WEBSERVER) {
        finishWifiScan();
        sendErrorResponse(503, F("设备内存不足，稍后再试"));
        return;
    }

    DynamicJsonDocument doc(1536);
    JsonObject data = doc.createNestedObject("data");
    data["status"] = "done";
    JsonArray networks = data.createNestedArray("networks");

    // SDK 返回的结果已按信号强度从强到弱排好，这里只截前 12 个
    int kept = 0;
    for (int i = 0; i < st && kept < 12; i++) {
        String name = WiFi.SSID(i);
        if (name.length() == 0) {
            continue;   // 隐藏网络取不到名字，手输即可
        }
        JsonObject item = networks.createNestedObject();
        item["ssid"] = name;
        item["rssi"] = WiFi.RSSI(i);
        item["encrypted"] = (WiFi.encryptionType(i) != ENC_TYPE_NONE);
        kept++;
    }

    finishWifiScan();   // 内含 scanDelete()：必须在读完结果之后
    sendJSONResponse(200, APIResponse::success(data, logger));
}

// 收尾：释放 SDK 的扫描结果 + 切回纯热点并重申热点参数。
// 幂等，超时兜底与正常路径都会走它。
void WebServer::finishWifiScan() {
    WiFi.scanDelete();   // 扫描结果占的是 SDK 的堆，必须显式释放
    wifiScanRunning = false;
    Serial.println(F("[配网] 扫描收尾：切回纯热点"));
    WiFi.mode(WIFI_AP);
    WiFiManager::configureAP();
}

// 回退到"上次真正连上 WiFi 的那份配置"（手机端的自救通道，与串口 AT+RESTORE 等价）。
// 固件里存的备份是「已知良好」而不是「上一版」——理由见 network_config.h 的说明。
void WebServer::handleAPIRestore() {
    if (!configManager.hasBackup()) {
        sendErrorResponse(404, F("没有可回退的备份（设备还没成功联过网）"));
        return;
    }

    const String backupSSID = configManager.getBackupSSID();
    if (!configManager.restoreFromBackup()) {
        sendErrorResponse(500, F("回退失败：配置写入出错"));
        return;
    }
    Serial.printf_P(PSTR("[配网] 已按用户请求回退到备份配置（SSID: %s）\n"), backupSSID.c_str());

    DynamicJsonDocument resp(256);
    JsonObject data = resp.createNestedObject("data");
    data["ssid"] = backupSSID;
    data["restartIn"] = 2;
    sendJSONResponse(200, APIResponse::success(data, logger));

    // 与 /save 同理：send() 只是塞进 lwIP 缓冲，必须留时间发出去，
    // 否则紧跟其后的 ESP.restart() 会让浏览器收到半截响应。
    for (int i = 0; i < 20; i++) {
        server.handleClient();
        yield();
        delay(100);
    }
    ESP.restart();
}

