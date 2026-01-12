#include <Arduino.h>
#include <ESP8266WiFi.h>

// 配置
#include "config/pins.h"
#include "config/constants.h"

// 工具
#include "utils/storage.h"
#include "utils/logger.h"
#include "utils/message_queue.h"
#include "utils/memory_monitor.h"

// 网络
#include "network/network_config.h"
#include "network/wifi_manager.h"
#include "network/mqtt_client.h"
#include "network/web_server.h"
#include "at_command/at_handler.h"

// 硬件
#include "hardware/led_control.h"
#include "hardware/ir_receiver.h"
#include "hardware/ir_transmitter.h"
#include "hardware/button_handler.h"

// 红外核心
#include "ir_core/ir_parser.h"
#include "ir_core/ir_mapper.h"
#include "ir_core/ir_forwarder.h"
#include "ir_core/ir_learner.h"

// HA接口
#include "ha_interface/ha_mqtt_handler.h"

// 全局对象
NetworkConfigManager networkConfig;
WiFiClient wifiClient;
WiFiManager wifiManager(networkConfig);
MQTTClient mqttClient(networkConfig, wifiClient);
WebServer webServer(networkConfig);
ATHandler atHandler(networkConfig);

LEDControl ledControl;
IRReceiver irReceiver;
IRTransmitter irTransmitter;
ButtonHandler buttonHandler;

IRParser irParser;
IRMapper irMapper;
MessageQueue forwardQueue;
IRForwarder irForwarder(irMapper, irTransmitter, forwardQueue);

Logger logger;
IRLearner irLearner(irReceiver, irParser, logger);
HAMQTTHandler haHandler(mqttClient, irMapper, irForwarder, logger);

// 任务定时器（使用毫秒时间戳）
unsigned long lastIRReceiverTime = 0;
unsigned long lastIRTransmitterTime = 0;
unsigned long lastMQTTTime = 0;
unsigned long lastNetworkTime = 0;
unsigned long lastButtonCheckTime = 0;

// 任务间隔（毫秒）
const unsigned long IR_RECEIVER_INTERVAL = 10;    // 10ms
const unsigned long IR_TRANSMITTER_INTERVAL = 10; // 10ms
const unsigned long MQTT_INTERVAL = 100;          // 100ms
const unsigned long NETWORK_INTERVAL = 500;       // 500ms
const unsigned long BUTTON_CHECK_INTERVAL = 20;  // 20ms

// 内存监控
unsigned long lastMemoryCheckTime = 0;
const float MEMORY_WARNING_THRESHOLD = 70.0f;  // 内存使用率超过70%时警告
const float MEMORY_CLEANUP_THRESHOLD = 85.0f;  // 内存使用率超过85%时开始清理（更积极的策略）
const float MEMORY_CRITICAL_THRESHOLD = 90.0f; // 内存使用率超过90%时强制清理
const unsigned long MEMORY_CHECK_INTERVAL = 30000; // 30秒检查一次内存（更频繁）

// Web服务器启动延迟（启动后等待时间，单位：毫秒）
const unsigned long WEB_SERVER_START_DELAY = 8000; // 8秒延迟
unsigned long systemStartTime = 0;

void setup() {
    Serial.begin(115200);
    Serial.println("\n\n=== ORVIBO-CT30W 红外转发器启动 ===");
    
    // 初始化内存监控（尽早初始化，记录初始堆大小）
    MemoryMonitor::init();
    
    // 初始化存储
    if (!Storage::init()) {
        Serial.println("LittleFS初始化失败！");
        while(1) delay(1000);
    }
    Serial.println("LittleFS初始化成功");
    
    // 初始化日志
    if (!logger.init()) {
        Serial.println("日志系统初始化失败！");
    }
    logger.log("system", "设备启动");
    
    // 初始化LED
    ledControl.init();
    ledControl.setState(LED_FAST_BLINK);
    
    // 初始化按钮处理器
    if (!buttonHandler.init()) {
        Serial.println("按钮处理器初始化失败！");
    }
    
    // 设置长按回调函数（重置配置）
    buttonHandler.setLongPressCallback([]() {
        Serial.println("\n[按钮] 检测到长按，开始重置配置...");
        
        // LED快速闪烁提示
        for (int i = 0; i < 10; i++) {
            ledControl.setState(LED_FAST_BLINK);
            delay(100);
            ledControl.setState(LED_OFF);
            delay(100);
        }
        
        // 清除所有配置
        networkConfig.reset();
        Serial.println("[按钮] 配置已清除");
        
        // LED常亮1秒
        ledControl.setState(LED_ON);
        delay(1000);
        
        // 重启设备
        Serial.println("[按钮] 设备即将重启...");
        delay(500);
        ESP.restart();
    });
    
    // 加载网络配置
    networkConfig.load();
    
    // 初始化映射表（在网络模块之前初始化，避免内存不足）
    if (!irMapper.init()) {
        Serial.println("映射表初始化失败！");
    }
    
    // 如果映射表为空，初始化默认映射关系（在网络模块之前，确保有足够内存）
    if (irMapper.getRuleCount() == 0) {
        Serial.println("映射表为空，正在初始化默认映射关系...");
        if (irMapper.initDefaultMappings()) {
            Serial.println("默认映射关系初始化成功");
            logger.log("system", "默认映射关系已初始化");
        } else {
            Serial.println("默认映射关系初始化失败");
            logger.logError("默认映射关系初始化失败");
        }
    }
    
    // 初始化WiFi
    if (!wifiManager.init()) {
        Serial.println("WiFi初始化失败，进入AP模式");
    }
    
    // 初始化MQTT
    mqttClient.init();
    
    // 初始化Web服务器（仅初始化路由，不启动）
    webServer.init();
    
    // 注入依赖到Web服务器
    webServer.setIRMapper(&irMapper);
    webServer.setIRForwarder(&irForwarder);
    webServer.setIRLearner(&irLearner);
    webServer.setLogger(&logger);
    
    // 记录系统启动时间（用于Web服务器延迟启动）
    systemStartTime = millis();
    
    // AP模式下立即启动Web服务器（配网需要）
    if (wifiManager.getStatus() == WIFI_AP_MODE) {
        webServer.start();
    }
    
    // 初始化AT指令处理器
    atHandler.init();
    
    // 初始化硬件
    if (!irReceiver.init()) {
        Serial.println("红外接收器初始化失败！");
        logger.logError("红外接收器初始化失败");
    }
    
    if (!irTransmitter.init()) {
        Serial.println("红外发射器初始化失败！");
        logger.logError("红外发射器初始化失败");
    }
    
    // 初始化红外核心
    if (!irParser.init()) {
        Serial.println("红外解析器初始化失败！");
    }
    
    if (!irForwarder.init()) {
        Serial.println("转发器初始化失败！");
    }
    
    irLearner.init();
    
    // 初始化HA接口
    haHandler.init();
    haHandler.setIRLearner(&irLearner);
    
    Serial.println("系统初始化完成！");
    
    // 初始化完成后关闭LED，进入待机状态
    ledControl.setState(LED_OFF);
    logger.log("system", "系统初始化完成");
}

void loop() {
    unsigned long now = millis();
    
    // 红外接收任务（最高优先级，10ms间隔）
    if (now - lastIRReceiverTime >= IR_RECEIVER_INTERVAL) {
        lastIRReceiverTime = now;
        
        // 学习模式下，跳过库解码，只使用自定义接收器
        if (!irLearner.isLearningDevice()) {
            // 使用IRremoteESP8266库解码（方案A：直接使用库）
            SignalID signalId;
            if (irParser.decodeWithLibrary(signalId)) {
                // 查找映射规则（用于转发）
                IR_MapRule* rule = irMapper.findRule(signalId);
                // 检查信号是否在映射表中
                // 如果已经匹配到A信号，直接设为matched；否则检查B信号
                bool matched = (rule != nullptr) || irMapper.isSignalInMappings(signalId);
                
                // 记录信号（包含匹配信息）
                logger.logReceiveSignal(signalId, matched);
                
                // LED闪烁
                ledControl.blink(1);
                
                // 查找映射并转发（优化：减少调试输出提升性能）
                // 只有作为A信号时才转发（避免循环转发）
                if (rule != nullptr) {
                    irForwarder.forwardSignal(signalId);
                }
            }
        }
        
        // 保留自定义接收器用于学习模式（需要原始时序数据）
        irReceiver.update();
    }
    
    // 红外发射任务（高优先级，优化：减少到5ms间隔提升响应速度）
    if (now - lastIRTransmitterTime >= 5) {  // 5ms间隔
        lastIRTransmitterTime = now;
        
        irForwarder.update();
        
        // 使用静态变量跟踪上次发射状态，只在状态变化时处理LED
        static bool lastTransmittingState = false;
        bool currentTransmittingState = irTransmitter.isTransmitting();
        
        if (currentTransmittingState) {
            ledControl.setState(LED_LONG_ON);
        } else if (lastTransmittingState && !currentTransmittingState) {
            // 发射状态从true变为false，发射完成
            // LED状态将由setNetworkState()在下次网络任务时更新为待机状态（关闭）
        }
        
        lastTransmittingState = currentTransmittingState;
    }
    
    // MQTT任务（中优先级，100ms间隔）
    if (now - lastMQTTTime >= MQTT_INTERVAL) {
        lastMQTTTime = now;
        haHandler.update();
    }
    
    // 网络任务（低优先级，500ms间隔）
    if (now - lastNetworkTime >= NETWORK_INTERVAL) {
        lastNetworkTime = now;
        
        wifiManager.update();
        mqttClient.update();
        
        // 更新LED网络状态
        bool wifiConnected = wifiManager.isConnected();
        bool mqttConnected = mqttClient.isConnected();
        bool mqttConfigured = networkConfig.isMQTTConfigured();
        ledControl.setNetworkStateWithMQTTConfig(wifiConnected, mqttConnected, mqttConfigured);
        
        // Web服务器条件启动/停止逻辑（优化：延迟启动，避免MQTT快速连接时不必要的启动）
        // 内存保护：内存不足时停止Web服务器
        bool shouldRunWebServer = false;
        unsigned long elapsedSinceStart = now - systemStartTime;
        float usagePercent = MemoryMonitor::getUsagePercent();
        
        // 内存不足时强制停止Web服务器
        // 检查条件：内存使用率超过92% 或 空闲内存小于1KB 或 最大连续块小于512字节
        // 调整阈值，在真正内存紧张时才停止Web服务器
        if (usagePercent > 92.0f || !MemoryMonitor::checkMemory(1024)) {
            if (webServer.isRunning()) {
                DEBUG_PRINTF("[Web服务器] 内存不足，停止Web服务器 (使用率: %.1f%%)\n", usagePercent);
                webServer.stop();
            }
            shouldRunWebServer = false;
        } else if (wifiManager.getStatus() == WIFI_AP_MODE) {
            // AP模式：内存充足时运行
            shouldRunWebServer = true;
        } else if (wifiConnected) {
            // WiFi已连接
            // 如果启动时间不足延迟时间，且MQTT已配置，等待MQTT连接成功
            if (elapsedSinceStart < WEB_SERVER_START_DELAY && mqttConfigured) {
                // 延迟期间：如果MQTT已连接且Discovery成功，则不启动Web服务器
                if (mqttConnected && haHandler.isDiscoveryPublished()) {
                    shouldRunWebServer = false;
                } else {
                    // 延迟期间：MQTT未就绪，暂时不启动（等待延迟结束）
                    shouldRunWebServer = false;
                }
            } else {
                // 延迟时间已过，按正常逻辑判断
                if (!mqttConfigured) {
                    // MQTT未配置：运行Web服务器
                    shouldRunWebServer = true;
                } else if (mqttConfigured && !mqttConnected) {
                    // MQTT已配置但连接失败：运行Web服务器
                    shouldRunWebServer = true;
                } else {
                    // MQTT已配置且已连接
                    // 检查 Discovery 是否成功发布，如果失败也启用 Web 服务器
                    bool discoverySuccess = haHandler.isDiscoveryPublished();
                    if (!discoverySuccess) {
                        // MQTT已连接但Discovery发布失败：运行Web服务器（用于排查问题）
                        shouldRunWebServer = true;
                    } else {
                        // MQTT已连接且Discovery成功：不运行Web服务器
                        shouldRunWebServer = false;
                    }
                }
            }
        }
        
        // 根据条件启动/停止Web服务器
        if (shouldRunWebServer && !webServer.isRunning()) {
            webServer.start();
        } else if (!shouldRunWebServer && webServer.isRunning()) {
            webServer.stop();
        }
        
        // 如果Web服务器正在运行，处理客户端请求
        if (webServer.isRunning()) {
            webServer.handleClient();
        }
    }
    
    // 主循环处理学习模式和AT指令
    irLearner.update();
    atHandler.update();
    
    // 按钮检测任务（低优先级，20ms间隔）
    if (now - lastButtonCheckTime >= BUTTON_CHECK_INTERVAL) {
        lastButtonCheckTime = now;
        buttonHandler.update();
    }
    
    // LED更新
    ledControl.update();
    
    // 内存监控和自动清理（每60秒检查一次）
    if (now - lastMemoryCheckTime >= MEMORY_CHECK_INTERVAL) {
        lastMemoryCheckTime = now;
        
        float usagePercent = MemoryMonitor::getUsagePercent();
        
        // 内存使用率超过警告阈值时打印警告（仅调试模式）
        if (usagePercent > MEMORY_WARNING_THRESHOLD) {
            DEBUG_PRINTF("[内存警告] 使用率: %.1f%%, 空闲: %d bytes, 最大连续块: %d bytes, 碎片率: %.1f%%\n",
                         usagePercent, MemoryMonitor::getFreeHeap(), 
                         MemoryMonitor::getLargestFreeBlock(), 
                         MemoryMonitor::getFragmentationPercent());
        }
        
        // 内存清理策略：85%开始清理，90%强制清理
        static bool lastMemoryCleanup = false;  // 跟踪上次是否处于清理状态
        bool currentMemoryCleanup = (usagePercent > MEMORY_CLEANUP_THRESHOLD);
        bool currentMemoryCritical = (usagePercent > MEMORY_CRITICAL_THRESHOLD);
        
        // 清理条件：当前是临界状态，或者连续两次检查都超过清理阈值
        if (currentMemoryCritical || (currentMemoryCleanup && lastMemoryCleanup)) {
            // 连续两次检查都超过阈值时才清理（避免频繁清理）
            DEBUG_PRINTLN("[内存清理] 内存使用率过高，开始自动清理...");
            
            // 1. 清理消息队列（释放队列中的任务）
            if (forwardQueue.size() > 0) {
                DEBUG_PRINTF("[内存清理] 清理消息队列 (%d 个任务)...\n", forwardQueue.size());
                forwardQueue.clear();
            }
            
            // 2. 清理部分日志（保留最近5条）
            if (logger.getLogCount() > 5) {
                DEBUG_PRINTF("[内存清理] 清理旧日志 (当前: %d 条)...\n", logger.getLogCount());
                logger.clearLogs();
            }
            
            // 3. 先保存日志（避免丢失重要信息）
            logger.flush();
            
            DEBUG_PRINTLN("[内存清理] 清理完成");
            
            lastMemoryCleanup = true;
        } else {
            lastMemoryCleanup = false;
        }
    }
    
    // 喂看门狗，避免看门狗复位（ESP8266默认看门狗超时约8秒）
    ESP.wdtFeed();
    
    // 短暂延迟，避免CPU占用过高
    delay(1);
}

