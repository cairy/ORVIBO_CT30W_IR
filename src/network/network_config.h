#ifndef NETWORK_CONFIG_H
#define NETWORK_CONFIG_H

#include <Arduino.h>

// 网络配置结构体
struct NetworkConfig {
    char wifi_ssid[32];
    char wifi_password[64];
    char mqtt_server[64];
    uint16_t mqtt_port;
    char mqtt_username[32];
    char mqtt_password[64];
    bool valid;
};

// 网络配置管理器
class NetworkConfigManager {
public:
    NetworkConfigManager();
    bool load();
    bool save();
    NetworkConfig& getConfig();
    void setWiFi(const String& ssid, const String& password);
    void setMQTT(const String& server, uint16_t port, 
                const String& username, const String& password);
    void clearMQTT();

    // ---- 入库前的长度校验（两个入口共用）-----------------------------------
    // 各字段都是**定长 char[]**，`strncpy(..., size-1)` 会**静默截断**：
    // 用户看到的是"保存成功"，代价是密码少一位 ⇒ 连不上 ⇒ 退 AP 模式 ⇒ 跑一趟现场
    // （设备可能吊在天花板上）。所以长度必须在入库前显式拦住并说清原因。
    //
    // 注意 WPA2-PSK 的两种合法形式：
    //   · 8~63 个可打印字符的口令 —— 32..63 位内
    //   · 64 位十六进制 PSK（本固件不支持，因为 mqtt/wifi 字段只留了 63+NUL）
    // 返回空串表示通过；否则返回给用户看的中文原因。
    //
    // enforceMinLength=false 用于"密码是从已存配置里沿用过来的"那种情况：
    // 老设备上可能存着 5 位 WEP 之类的不合规口令，重新保存一条无关改动
    // （比如只改 MQTT）不该因为旧密码太短而被拦住 —— 那会把用户锁在门外。
    // 只有"用户这次真的手打了密码"才强制 8 位下限。
    static String checkWiFi(const String& ssid, const String& password,
                            bool enforceMinLength = true);
    static String checkMQTT(const String& server, uint16_t port,
                            const String& username, const String& password);

    bool isValid() const;
    bool isMQTTConfigured() const;
    void reset();

    // ---- 配置双备份（v1.3.9）------------------------------------------------
    // 备份文件里存的是「**已知良好**配置」——最后一次真正连上 WiFi 的那一份，
    // 由 main.cpp 在 WiFi 由断线转为连上时调用 markKnownGood() 推进。
    //
    // 为什么不是「上一版」：用户在 AP 配网页填错 → 保存（上一版=正确的）→ 再填对 → 保存
    // （上一版=错的），备份就被污染了。存「已验证能联网的那版」才救得回来。
    //
    // 于是有两种救法：
    //   · 主配置写坏 / 解析失败 → 启动时自动回退（load() 内部完成，并写回主配置）
    //   · 主配置内容写错（如 WiFi 密码填错，设备连不上退 AP 模式）
    //                           → 串口 `AT+RESTORE` 手动回退到已知良好配置
    bool hasBackup() const;
    bool restoreFromBackup();   // 从备份恢复到内存并写回主配置；成功返回 true
    void markKnownGood();       // 把当前内存配置存为已知良好（联网成功后调用）
    String getBackupSSID() const;  // 备份里的 SSID（供 AT+QUERY 显示，让用户确认该不该回退）

private:
    NetworkConfig config;
    static const char* CONFIG_FILE;
    static const char* BACKUP_FILE;
    bool loadFromFlash();
    bool saveToFlash();
    bool loadFromFile(const char* path);          // 解析指定文件到 config
    bool writeConfigTo(const char* path);         // 把 config 写入指定文件
};

#endif // NETWORK_CONFIG_H

