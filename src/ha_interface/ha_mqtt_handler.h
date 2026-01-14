#ifndef HA_MQTT_HANDLER_H
#define HA_MQTT_HANDLER_H

#include <Arduino.h>
#include "network/mqtt_client.h"
#include "ir_core/ir_mapper.h"
#include "ir_core/ir_forwarder.h"
#include "ir_core/ir_learner.h"
#include "utils/logger.h"
#include "utils/api_response.h"
#include "config/constants.h"

// HA MQTT 指令处理器
class HAMQTTHandler : public APIResponseHandler {
public:
    HAMQTTHandler(MQTTClient& mqtt, IRMapper& mapper, 
                  IRForwarder& forwarder, Logger& logger);
    bool init();
    void update();
    void handleCommand(const String& topic, const String& payload);
    void handleCommand(const char* topic, const char* payload, size_t payloadLen);  // 优化：避免String对象
    void publishState(const String& state, const String& message);
    void publishLog(const String& logEntry);
    void setIRLearner(IRLearner* learner) { this->learner = learner; }
    bool isDiscoveryPublished() const { return discoveryPublished; }
    void publishEnhancedState();  // 发布增强的状态信息
    
    // APIResponseHandler 接口实现
    bool sendSuccess(const String& json, const char* context = nullptr) override;
    bool sendError(const String& error, int code = 0, const char* context = nullptr) override;
    Logger* getLogger() override { return &logger; }

private:
    MQTTClient& mqtt;
    IRMapper& mapper;
    IRForwarder& forwarder;
    Logger& logger;
    IRLearner* learner;
    bool discoveryPublished;
    unsigned long lastDiscoveryAttempt;
    unsigned long firstConnectedTime;  // MQTT首次连接时间
    int discoveryRetryCount;  // Discovery重试次数
    unsigned long lastStatePublishTime;  // 上次状态发布时间
    
    // 状态缓存（使用固定大小数组，避免String的动态内存分配）
    char lastPublishedStatus[16];  // 上次发布的状态（"ready"/"learning"/"error"）
    bool lastPublishedForwardingEnabled;  // 上次发布的转发状态
    char lastPublishedLearnMode[16];  // 上次发布的学习模式（"none"/"device"）
    int lastPublishedRuleCount;  // 上次发布的规则数量
    bool stateCacheInitialized;  // 状态缓存是否已初始化
    
    void processCommand(const String& payload);
    void publishDiscoveryConfig();
    void handleQueryMaps();
    void handleUpdateMap(const String& json);  // 支持单个和批量更新
    void handleGetLogs();
    void handleStartLearn(const String& json);
    void handleQueryLearnStatus(const String& json);
    void handleStopLearn(const String& json);
    void handleQueryLearnResult(const String& json);
    void handleGetRecentSignals(const String& json);
    void handleSendSignal(const String& json);
    void handleSetForwardingEnabled(const String& json);
    void handleGetStatus();  // 获取设备状态
    void handleSmartIRSignal(const String& payload);  // 处理 SmartIR 发送的信号（Base64/数组/协议格式）
    
    static void mqttCallback(char* topic, byte* payload, unsigned int length);
    static HAMQTTHandler* instance;
};

#endif // HA_MQTT_HANDLER_H

