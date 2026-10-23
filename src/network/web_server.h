#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <Arduino.h>
#include <functional>
#include <ESP8266WebServer.h>
#include "network_config.h"
#include "config/constants.h"
#include "utils/api_response.h"

// 前向声明
class IRMapper;
class IRForwarder;
class IRLearner;
class Logger;
class MQTTClient;

// Web 服务器（AP 配网界面 + HTTP REST API）
class WebServer : public APIResponseHandler {
public:
    WebServer(NetworkConfigManager& config);
    bool init();
    void handleClient();
    void stop();
    void start();
    bool isRunning() const;
    
    // 暴露底层 HTTP 服务实例，供 OTA 挂载 /update 等额外路由
    ESP8266WebServer& httpServer() { return server; }
    
    // 依赖注入方法
    void setIRMapper(IRMapper* mapper) { this->mapper = mapper; }
    void setIRForwarder(IRForwarder* forwarder) { this->forwarder = forwarder; }
    void setIRLearner(IRLearner* learner) { this->learner = learner; }
    void setLogger(Logger* logger) { this->logger = logger; }
    void setMQTTClient(MQTTClient* mqtt) { this->mqtt = mqtt; }

private:
    ESP8266WebServer server;
    NetworkConfigManager& configManager;
    bool running;
    
    // 周边 WiFi 扫描的状态。扫描改成两段式（见 handleAPIWifiScan 的注释）：
    // 第一段只负责"起扫"并立刻返回，第二段取结果。中间这段时间主循环照常跑。
    // 这里的超时兜底很重要：用户点了"搜索"然后直接关掉手机页面，就再也没人
    // 来取结果了，不留兜底设备会永远停在"热点+站点"双模上。
    bool wifiScanRunning;
    unsigned long wifiScanStartedAt;
    void finishWifiScan();          // 收尾：释放 SDK 结果 + 切回纯热点
    
    // 依赖对象指针
    IRMapper* mapper;
    IRForwarder* forwarder;
    IRLearner* learner;
    Logger* logger;
    MQTTClient* mqtt;
    
    // 页面：内置 SPA（/）、配网页（/setup，与面板同一套配色）与保存结果页
    void handleRoot();
    void handleSetupPage();
    void handleSave();
    // 保存/恢复之后的收尾页（ok=false 时不带重启倒计时）
    void sendOutcomePage(bool ok, const String& heading, const String& detail);
    // 发送 gzip 压缩的内置资源；浏览器不支持 gzip 时返回 406 说明
    void sendAsset_P(int code, PGM_P contentType, const uint8_t* data, size_t len);
    
    // HTTP API路由处理
    void handleAPIStatus();
    void handleAPIMaps();
    void handleAPIUpdateMap();
    void handleAPILogs();
    void handleAPILearnStart();
    void handleAPILearnStatus();
    void handleAPILearnStop();
    void handleAPILearnResult();
    void handleAPIClearLearnResult();   // DELETE /api/learn/result（v1.3.50）
    void handleAPIRecentSignals();
    void handleAPIClearRecentSignals();
    void handleAPISendSignal();
    void handleAPISetForwarding();
    void handleAPIGetForwardRecords();
    void handleAPIForwardTest();
    void handleAPIClearForwardRecords();
    void handleAPIGetDevices();
    void handleAPISaveDevices();
    // v1.3.42：分片提交设备配置（整份 POST 在堆紧时会被框架丢连接，见 .cpp 里的说明）
    void handleAPISaveDevicesChunk();
    // 网络僵死现场（v1.3.36）：设备因网络僵死自愈重启后，可从这里取回重启前的快照。
    // GET 读快照、DELETE 清掉；诊断入口，走 onGuarded 与其余 API 一致。
    void handleAPIDiagFreeze();
    void handleAPIClearDiagFreeze();
    void handleAPIDiagCrash();        // 崩溃现场（v1.3.50）
    void handleAPIClearDiagCrash();
    // 仅调试：强制让看门狗接下来几次探测判为超时，用来验证"快照落盘 → 重启 → 读回"链路。
    void handleAPIWifiScan();
    void handleAPIWifiScanResult();
    void handleAPIRestore();
    
    // 辅助方法
    void sendJSONResponse(int code, const String& json);
    void sendErrorResponse(int code, const String& error);
    void addCORSHeaders();
    // Host 头白名单（挡 DNS rebinding / 跨站读取）：只认设备自己的 STA 与 AP 地址
    bool hostAllowed(const String& host);

    // 受保护路由的统一前置检查：Host 白名单 + 可选 Basic 认证。失败时自己发响应。
    bool guardRequest();
    void onGuarded(const char* uri, HTTPMethod method, std::function<void()> fn);
    
    // 内存检查辅助方法
    bool checkMemoryBeforeAPI(size_t requiredBytes = 2048);
    
    // APIResponseHandler 接口实现
    bool sendSuccess(const String& json, const char* context = nullptr) override;
    bool sendError(const String& error, int code = 0, const char* context = nullptr) override;
    Logger* getLogger() override { return logger; }
};

#endif // WEB_SERVER_H

