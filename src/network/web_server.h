#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <Arduino.h>
#include <ESP8266WebServer.h>
#include "network_config.h"
#include "config/constants.h"
#include "utils/api_response.h"

// 前向声明
class IRMapper;
class IRForwarder;
class IRLearner;
class Logger;

// Web 服务器（AP 配网界面 + HTTP REST API）
class WebServer : public APIResponseHandler {
public:
    WebServer(NetworkConfigManager& config);
    bool init();
    void handleClient();
    void stop();
    void start();
    bool isRunning() const;
    
    // 依赖注入方法
    void setIRMapper(IRMapper* mapper) { this->mapper = mapper; }
    void setIRForwarder(IRForwarder* forwarder) { this->forwarder = forwarder; }
    void setIRLearner(IRLearner* learner) { this->learner = learner; }
    void setLogger(Logger* logger) { this->logger = logger; }

private:
    ESP8266WebServer server;
    NetworkConfigManager& configManager;
    bool running;
    
    // 依赖对象指针
    IRMapper* mapper;
    IRForwarder* forwarder;
    IRLearner* learner;
    Logger* logger;
    
    // 配网界面处理
    void handleRoot();
    void handleConfig();
    void handleSave();
    String getHTML();
    
    // HTTP API路由处理
    void handleAPIStatus();
    void handleAPIMaps();
    void handleAPIUpdateMap();
    void handleAPILogs();
    void handleAPILearnStart();
    void handleAPILearnStatus();
    void handleAPILearnStop();
    void handleAPILearnResult();
    void handleAPIRecentSignals();
    void handleAPISendSignal();
    
    // 辅助方法
    void sendJSONResponse(int code, const String& json);
    void sendErrorResponse(int code, const String& error);
    void addCORSHeaders();
    
    // 内存检查辅助方法
    bool checkMemoryBeforeAPI(size_t requiredBytes = 2048);
    
    // APIResponseHandler 接口实现
    bool sendSuccess(const String& json, const char* context = nullptr) override;
    bool sendError(const String& error, int code = 0, const char* context = nullptr) override;
    Logger* getLogger() override { return logger; }
};

#endif // WEB_SERVER_H

