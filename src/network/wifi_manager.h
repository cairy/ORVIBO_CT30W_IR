#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include "network_config.h"
#include "config/constants.h"

// WiFi 连接状态
enum WiFiStatus {
    WIFI_DISCONNECTED,
    WIFI_AP_MODE,
    WIFI_CONNECTING,
    WIFI_CONNECTED
};

// WiFi 管理器
class WiFiManager {
public:
    WiFiManager(NetworkConfigManager& config);
    bool init();
    void update();
    WiFiStatus getStatus() const;
    bool isConnected() const;
    String getIP() const;
    void startAP();
    void stopAP();
    bool connect(const String& ssid, const String& password);
    void disconnect();

    // 用项目固定参数（AP_SSID / AP_PASSWORD / 192.168.4.1）打开热点。
    // 做成静态函数是因为配网页扫描周边 WiFi 时会短暂切到"热点+站点"双模，
    // 切回来必须重申一次热点参数，而那一处（web_server）手里没有 WiFiManager 实例。
    static void configureAP();

private:
    NetworkConfigManager& configManager;
    WiFiStatus status;
    unsigned long lastReconnectAttempt;
    int reconnectAttempts;
    bool apMode;

    // 非阻塞重连：发起一次 WiFi.begin() 后只记时间戳，之后每次 update() 查一次结果，
    // 绝不在主循环里 delay 等待（那会让红外转发停摆，还把看门狗喂饱）。
    bool reconnectInProgress;
    unsigned long reconnectStartedAt;

    // AP 模式下的 STA 回探：路由器重启一次就可能把设备推进 AP，若不再回探，
    // 网络恢复了设备也永远留在 AP 里 ⇒ 红外转发永久停摆。
    bool staProbeInProgress;
    unsigned long staProbeStartedAt;
    unsigned long lastStaProbe;

    // ── 掉线现场记录（v1.3.31 埋点，只加日志、不改行为）────────────────────
    // 「多发几次就失效」的真身不是发射（v1.3.30 已证：200 连打零异常），而是
    // 设备**先掉 WiFi**，随后的 10 次重连把堆从 ~9 KB 啃到几百字节 ⇒ Exception 29 ⇒ WDT。
    // 掉线原因（802.11 reason 码）只有事件回调拿得到，而**回调跑在 SDK 系统任务上下文**，
    // 那里栈很小 —— 所以在回调里**只做赋值**，printf 一律放到 update() 的 loop 上下文。
    volatile bool      discPending;    // 有一次尚未打印的掉线事件
    volatile int       discReason;     // 802.11 reason 码
    volatile uint8_t   discStatusCode; // WiFi.status() 快照
    String             discSsid;       // 掉线时报的 SSID（loop 上下文里才会读到）
    WiFiEventHandler   discHandler;    // 必须存住，否则 shared_ptr 析构即自动退订
    void pollWifiEventLog();

    void handleReconnect();
    void probeStaFromAP(unsigned long now);
};

#endif // WIFI_MANAGER_H

