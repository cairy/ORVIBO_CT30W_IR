#include "wifi_manager.h"

WiFiManager::WiFiManager(NetworkConfigManager& config) 
    : configManager(config), status(WIFI_DISCONNECTED), 
      lastReconnectAttempt(0), reconnectAttempts(0), apMode(false),
      reconnectInProgress(false), reconnectStartedAt(0),
      staProbeInProgress(false), staProbeStartedAt(0), lastStaProbe(0),
      discPending(false), discReason(0), discStatusCode(0) {
}

bool WiFiManager::init() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);

    // ── WiFi 事件埋点（v1.3.31，只加日志）───────────────────────────────
    // 目的：抓住"掉线的**原因**"，以及掉线那一刻堆还剩多少。
    //   · 堆还很健康就掉线 ⇒ 外部原因（AP 踢人 / 信标丢失），重连循环才是凶手；
    //   · 堆已经见底才掉线 ⇒ WiFi 驱动自己分配失败，根子在堆。
    // reason 是 802.11 状态码：2=AUTH_EXPIRE 4=DISASSOC_INACTIVITY 8=ASSOC_LEAVING
    //   15=4WAY_HANDSHAKE_TIMEOUT 200=BEACON_TIMEOUT 201=NO_AP_FOUND
    //   202=AUTH_FAIL 203=ASSOC_FAIL 204=HANDSHAKE_TIMEOUT
    // ⚠️ 用 onStationModeDisconnected（不是已废弃的 onEvent）—— 只有它带 reason。
    // ⚠️⚠️ 回调里**绝不能 Serial.printf**：事件回调在 SDK 系统任务上下文执行，
    //   那个任务栈很小，printf 的 varargs 有把系统栈打爆的风险（比不打日志还糟）。
    //   所以这里只赋值，打印交给 update() → pollWifiEventLog()。
    // ⚠️ 返回的 handler 必须存住：shared_ptr 一析构就自动退订，日志会静默消失。
    discHandler = WiFi.onStationModeDisconnected(
        [this](const WiFiEventStationModeDisconnected& info) {
            discReason     = (int)info.reason;
            discStatusCode = (uint8_t)WiFi.status();
            discSsid       = info.ssid;
            discPending    = true;
        });

    if (configManager.isValid()) {
        NetworkConfig& config = configManager.getConfig();
        return connect(config.wifi_ssid, config.wifi_password);
    } else {
        startAP();
        return true;
    }
}

void WiFiManager::update() {
    unsigned long now = millis();

    pollWifiEventLog();   // 埋点（v1.3.31）：把事件回调记下的掉线原因打出来

    if (apMode) {
        // AP 模式不是终点站 —— 见 probeStaFromAP()。
        // 以前这里直接 return，于是设备一旦进 AP 就再也回不到 STA：
        // 唯一会清 apMode 的 connect() 只有 init() 一处调用。
        probeStaFromAP(now);
        return;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        if (status != WIFI_CONNECTED) {
            status = WIFI_CONNECTED;
            reconnectAttempts = 0;
            Serial.println(F("WiFi connected"));
        }
    } else {
        if (status == WIFI_CONNECTED) {
            status = WIFI_DISCONNECTED;
            Serial.println(F("WiFi disconnected"));
        }
        handleReconnect();
    }
}

WiFiStatus WiFiManager::getStatus() const {
    return status;
}

// ── 埋点（v1.3.31，只加日志）：打印掉线现场 ──────────────────────────────
// 放在 loop 上下文的理由见 init() 里那段注释（事件回调栈太小，不能 printf）。
// heap/blk 是**打印这一刻**的值，与事件只差几毫秒；配合"重连第 N 次时的堆"一起看，
// 就能分清是"掉线时堆本来就没剩多少"还是"重连循环把堆啃光了"。
void WiFiManager::pollWifiEventLog() {
    if (!discPending) return;
    discPending = false;
    Serial.printf_P(PSTR("[wifi] 断开 reason=%d ssid=%s status=%u heap=%u blk=%u\n"),
                  (int)discReason, discSsid.c_str(), (unsigned)discStatusCode,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize());
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

void WiFiManager::configureAP() {
    // 配置AP的IP地址、网关和子网掩码
    IPAddress apIP(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(apIP, gateway, subnet);

    // 启动AP模式（幂等：重复调用只是重申一遍同样的参数）
    WiFi.softAP(AP_SSID, AP_PASSWORD);
}

void WiFiManager::startAP() {
    WiFi.mode(WIFI_AP);
    configureAP();
    
    apMode = true;
    status = WIFI_AP_MODE;
    
    // 等待AP启动完成
    delay(100);
    
    // 安全地获取IP地址字符串（避免临时对象问题）
    String ipStr = WiFi.softAPIP().toString();
    Serial.print(F("AP mode started: SSID="));
    Serial.print(AP_SSID);
    Serial.print(F(", IP="));
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
        Serial.print(F("."));
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        status = WIFI_CONNECTED;
        // 安全地获取IP地址字符串（避免临时对象问题）
        String ipStr = WiFi.localIP().toString();
        Serial.print(F("\nWiFi connected: "));
        Serial.println(ipStr);
        return true;
    } else {
        status = WIFI_DISCONNECTED;
        Serial.println(F("\nWiFi connection failed"));
        startAP();
        return false;
    }
}

void WiFiManager::disconnect() {
    WiFi.disconnect();
    status = WIFI_DISCONNECTED;
}

void WiFiManager::handleReconnect() {
    unsigned long now = millis();

    // 上一次发起的连接还在等结果：只查状态，不等待。
    // ⚠️ 旧实现是 `while (WiFi.status() != WL_CONNECTED && attempts < 10) delay(500);`
    // —— 在主循环里一次忙等 5 秒，而 lastReconnectAttempt 是在忙等**之前**赋值的，
    // 忙等回来又立刻满足间隔 ⇒ 连续 10 次无缝衔接，约 50 秒内红外接收/发射、
    // MQTT、面板全部停摆；delay() 还把看门狗喂得饱饱的，SafeMode 也不计数
    // ⇒ 设备表现为"按遥控器没反应、串口安静"，且空中无从自救。
    if (reconnectInProgress) {
        if (WiFi.status() == WL_CONNECTED) {
            reconnectInProgress = false;
            status = WIFI_CONNECTED;
            reconnectAttempts = 0;
            Serial.println(F("WiFi reconnected"));
        } else if (now - reconnectStartedAt >= WIFI_RECONNECT_TIMEOUT_MS) {
            reconnectInProgress = false;
            status = WIFI_DISCONNECTED;
            // 埋点（v1.3.31）：每次重连失败都带上堆 —— 这是"重连是否在啃堆"的直接证据。
            Serial.printf_P(PSTR("WiFi reconnect attempt %d/%d timed out (heap=%u blk=%u)\n"),
                          reconnectAttempts, WIFI_RECONNECT_MAX_ATTEMPTS,
                          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize());
            if (reconnectAttempts >= WIFI_RECONNECT_MAX_ATTEMPTS) {
                Serial.println(F("Max reconnect attempts reached, starting AP mode"));
                reconnectAttempts = 0;
                startAP();
            }
        }
        return;
    }

    if (now - lastReconnectAttempt < WIFI_RECONNECT_DELAY_MS) {
        return;
    }
    lastReconnectAttempt = now;

    if (!configManager.isValid()) {
        return;
    }

    reconnectAttempts++;
    // 埋点（v1.3.31）：带上发起时的堆 ⇒ 与上面那条"超时"行配对，一眼看出每轮重连啃掉多少。
    Serial.printf_P(PSTR("Attempting WiFi reconnect (%d/%d)... heap=%u blk=%u\n"),
                  reconnectAttempts, WIFI_RECONNECT_MAX_ATTEMPTS,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize());

    // 埋点（v1.3.36）：把 `WiFi.begin()` **本身**的分配单独量出来。
    // 起因：2026-10-03 13:51 串口实测，每轮"断开 → 重连超时"固定吃掉 **2024 B**
    // （5528 → 3504 → 1480），三轮把 12 KB 堆抽干 ⇒ Exception 29 崩溃。
    // 结论（v1.3.37 实测推翻了"重连漏 2 KB"这个归因）：
    //   · begin() 本身只 −184 B，**且立刻归还**，多轮重复也不累积；
    //   · 那 2024 B 不在我们代码里 —— 崩溃栈全是 SDK 的 802.11：
    //     ieee80211_parse_beacon / beacon_alloc / alloc_proberesp / sta_new_state /
    //     wpa_supplicant_parse_ies，且崩在 **sys 上下文**（WiFi 驱动任务，不是主循环）；
    //   · 触发条件是 5 秒窗口下**上一轮 STA 状态机还没走完就重入** connect()，
    //     v1.3.37 把窗口提到 15 秒后，连续 6 轮失败实测 blk 恒定 5488、堆不降反升。
    // 埋点保留：它现在的作用是"一眼分清 2 KB 是 SDK 欠的还是我们欠的"。
    const uint32_t heapBeforeBegin = ESP.getFreeHeap();
    const uint32_t blkBeforeBegin  = ESP.getMaxFreeBlockSize();

    NetworkConfig& config = configManager.getConfig();
    WiFi.begin(config.wifi_ssid, config.wifi_password);

    const uint32_t heapAfterBegin = ESP.getFreeHeap();
    const uint32_t blkAfterBegin  = ESP.getMaxFreeBlockSize();
    Serial.printf_P(PSTR("[wifi] begin() 本身: heap %u→%u (d=%d) blk %u→%u (d=%d)\n"),
                  (unsigned)heapBeforeBegin, (unsigned)heapAfterBegin,
                  (int)heapAfterBegin - (int)heapBeforeBegin,
                  (unsigned)blkBeforeBegin, (unsigned)blkAfterBegin,
                  (int)blkAfterBegin - (int)blkBeforeBegin);

    reconnectInProgress = true;
    reconnectStartedAt = now;
}

void WiFiManager::probeStaFromAP(unsigned long now) {
    // 设备是吊在天花板/柜子里的：路由器重启一次、或 Wi-Fi 抖一下，
    // 10 次重连失败就会把设备推进 AP 模式。若就此不再回探，即使网络恢复，
    // 设备也永远停在 AP 里 —— 红外转发永久停摆，只能人工爬上去按。
    // 所以 AP 模式下也保留一次低频 STA 回探（60 秒一次，同样非阻塞）。
    if (!configManager.isValid()) {
        return;
    }

    if (staProbeInProgress) {
        if (WiFi.status() == WL_CONNECTED) {
            staProbeInProgress = false;
            Serial.println(F("STA 回探成功，退出 AP 模式，红外转发恢复"));
            stopAP();                 // 内部会把 apMode 置 false 并切回 WIFI_STA
            status = WIFI_CONNECTED;
            reconnectAttempts = 0;
        } else if (now - staProbeStartedAt >= WIFI_STA_PROBE_TIMEOUT_MS) {
            staProbeInProgress = false;
            // 回探失败：WiFi.begin() 会把模式切走，这里切回纯 AP 并重申热点参数
            // （configureAP() 是幂等的）
            WiFi.mode(WIFI_AP);
            configureAP();
            Serial.println(F("STA 回探失败，继续留在 AP 模式"));
        }
        return;
    }

    if (lastStaProbe == 0) {
        lastStaProbe = now;   // 刚进 AP 就回探没意义，先等一个周期
        return;
    }
    if (now - lastStaProbe < AP_STA_PROBE_INTERVAL_MS) {
        return;
    }

    // 别在"周边 WiFi 搜索"（WebServer::handleAPIWifiScan 起的异步扫描）期间回探：
    // 下面的 WiFi.begin() 会打断正在进行的扫描，而那边的 /api/wifi/scan/result
    // 正等着结果。SDK 自己就知道有没有在扫，不需要两边互相持有引用。
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
        return;   // 下一个周期再说
    }

    lastStaProbe = now;

    NetworkConfig& config = configManager.getConfig();
    // 注意：切到 AP+STA 双模的瞬间，正在连着热点配网的手机可能会短暂掉线。
    // 60 秒一次、且失败立刻切回，可以接受 —— 比起"永久停在 AP"划算得多。
    WiFi.mode(WIFI_AP_STA);
    // 埋点（v1.3.36）：这条路径同样在调 WiFi.begin()，但**多了一次模式切换**
    // （失败后 `WiFi.mode(WIFI_AP)` 会把 STA 拆掉）—— 实测每轮结束堆都精确回到原值
    // （8360 → 8360），是唯一一条"失败也不留债"的重连路径。留着做对照。
    const uint32_t heapBeforeProbe = ESP.getFreeHeap();
    WiFi.begin(config.wifi_ssid, config.wifi_password);
    Serial.printf_P(PSTR("[wifi] AP回探 begin() 本身: heap %u→%u (d=%d)\n"),
                  (unsigned)heapBeforeProbe, (unsigned)ESP.getFreeHeap(),
                  (int)ESP.getFreeHeap() - (int)heapBeforeProbe);
    staProbeInProgress = true;
    staProbeStartedAt = now;
}

