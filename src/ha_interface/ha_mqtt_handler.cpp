#include "ha_mqtt_handler.h"
#include "ir_core/ir_learner.h"
#include "utils/hex_utils.h"
#include "utils/memory_monitor.h"
#include "utils/api_response.h"
#include "utils/unified_api.h"
#include "utils/api_handlers.h"
#include "utils/device_state.h"
#include "config/constants.h"
#include <ArduinoJson.h>

HAMQTTHandler* HAMQTTHandler::instance = nullptr;

// MQTT 命令报文的最大处理长度（超出部分截断）——沿用历史上 1024 B 的上限，
// 避免超大报文在本就紧张的堆上构造 String。
static const size_t MAX_MQTT_PAYLOAD_LEN = 1024;

// ── 订阅状态表（每个主题一份）─────────────────────────────────────────────
// ⚠️ 以前这里是 `bool ok = subscribe(命令主题); ok &= subscribe(气候); ok &= ...;`
// 再把结果存进一个 `subscribed` 变量 —— **全或无**。后果有两条：
//   ① 4 个 SmartIR 通配主题只要有一个被 broker 拒（不允许通配订阅的 broker 很常见），
//      subscribed 永远是 false ⇒ 那个 `if (... || !subscribed)` 每 2 秒把所有主题
//      全量重订一遍，**一辈子不停**，在只有几 KB 空闲堆的设备上纯属自残；
//   ② 更糟的是它**掩盖事实**：命令主题其实早就订上了，但所有线索都指向"订阅失败"。
// 现在逐主题记账：订上的不再重订；没订上的按各自上限重试。
// "还需不需要再订"由 `hasPendingSubscription()` 直接算出来 —— 那个 `subscribed` 变量已经删掉，
// 它在新设计里从未被读过，只留下一个 set-but-unused 编译警告。
struct MqttSubSlot {
    const char* topic;
    bool ok;
    uint8_t attempts;
    uint8_t maxAttempts;   // 命令主题是必需的，多给几次；SmartIR 是加分项，少给几次
};

static MqttSubSlot subSlots[] = {
    { MQTT_TOPIC_COMMAND,            false, 0, 30 },
    { "smartir/climate/+/send",      false, 0, 5 },
    { "smartir/media_player/+/send", false, 0, 5 },
    { "smartir/fan/+/send",          false, 0, 5 },
    { "smartir/light/+/send",        false, 0, 5 },
};
static const size_t SUB_SLOT_COUNT = sizeof(subSlots) / sizeof(subSlots[0]);

static bool hasPendingSubscription() {
    for (size_t i = 0; i < SUB_SLOT_COUNT; i++) {
        if (!subSlots[i].ok && subSlots[i].attempts < subSlots[i].maxAttempts) return true;
    }
    return false;
}

static void resetSubscriptions() {
    for (size_t i = 0; i < SUB_SLOT_COUNT; i++) {
        subSlots[i].ok = false;
        subSlots[i].attempts = 0;
    }
}

// ── Home Assistant Discovery 配置模板（v1.3.17）─────────────────────────
// 4 条配置里**唯一会变的只有 chipId**（且只出现在 device.identifiers 里），其余全是编译期常量。
// 原先用 4 个 DynamicJsonDocument + serializeJson 在堆上现拼，于是 publishDiscoveryConfig()
// 只能靠开头的 `freeHeap < 4096 ⇒ 跳过` 自保；而本机**配了 MQTT 之后空闲堆只有 ~4.0 KB**
// （8 KB 收发缓冲的代价），这个门槛把整段恒久跳过 ⇒ Discovery 从未真正发布过。
// 现在改成 PROGMEM 常量 + 逐字节拼进栈缓冲：**零堆分配**，门槛自然可以拿掉；
// 模板本身也不再占 RAM（这个 core 里 .rodata 落在 dram0_0_seg，普通字符串常量是吃 RAM 的）。
//
// 注意：本 core 的 newlib 已不再提供 snprintf_P / strcpy_P / memcpy_P，只有 pgm_read_byte，
// 所以下面统一用 appendPgmStr / appendRamStr 拼串，不依赖任何 _P 函数族。
static const char DISCOVERY_DEVICE_HEAD[] PROGMEM =
    "\"device\":{\"identifiers\":[\"" MQTT_DEVICE_ID "_";
static const char DISCOVERY_DEVICE_TAIL[] PROGMEM =
    "\"],\"name\":\"" MQTT_DEVICE_NAME "\",\"model\":\"ORVIBO CT30W\","
    "\"manufacturer\":\"ORVIBO\",\"sw_version\":\"" FW_VERSION "\"}";

static const char DISCOVERY_STATUS_HEAD[] PROGMEM =
    "{\"name\":\"" MQTT_DEVICE_NAME " 状态\",\"unique_id\":\"" MQTT_DEVICE_ID "_status\","
    "\"state_topic\":\"" MQTT_TOPIC_STATE "/status\",\"device_class\":\"connectivity\","
    "\"payload_on\":\"online\",\"payload_off\":\"offline\",";

static const char DISCOVERY_STATE_HEAD[] PROGMEM =
    "{\"name\":\"" MQTT_DEVICE_NAME " 信息\",\"unique_id\":\"" MQTT_DEVICE_ID "_state\","
    "\"state_topic\":\"" MQTT_TOPIC_STATE "\",\"value_template\":\"{{ value_json.status }}\","
    "\"json_attributes_topic\":\"" MQTT_TOPIC_STATE "\","
    "\"json_attributes_template\":\"{{ value_json | tojson }}\",";

static const char DISCOVERY_LOG_HEAD[] PROGMEM =
    "{\"name\":\"" MQTT_DEVICE_NAME " 日志\",\"unique_id\":\"" MQTT_DEVICE_ID "_log\","
    "\"state_topic\":\"" MQTT_TOPIC_LOG "\",\"icon\":\"mdi:text-box\",";

static const char DISCOVERY_SWITCH_HEAD[] PROGMEM =
    "{\"name\":\"" MQTT_DEVICE_NAME " 转发开关\",\"unique_id\":\"" MQTT_DEVICE_ID "_forwarding_enabled\","
    "\"state_topic\":\"" MQTT_TOPIC_STATE "\","
    // 模板里的字段名必须与 payload 里的键一致 —— 这是两份**分开维护**的字符串，
    // 只改一处 HA 就永远读不到开关状态（模板恒 fallback 到 'false'，看起来像"转发被关了"）。
    // 注意 unique_id 与 discovery 主题里的 `forwarding_enabled` 是**实体标识**，不跟着改：
    // 改了 HA 会当成新实体，用户现有的卡片/自动化会指向一个不再更新的旧实体。
    "\"value_template\":\"{{ 'true' if (value_json.data is mapping and value_json.data.forwardingEnabled is defined and value_json.data.forwardingEnabled) else 'false' }}\","
    "\"command_topic\":\"" MQTT_TOPIC_COMMAND "\","
    "\"payload_on\":\"true\",\"payload_off\":\"false\",\"state_on\":\"true\",\"state_off\":\"false\","
    "\"icon\":\"mdi:swap-horizontal\",";

// 4 条配置都以 "}" 收尾（中间嵌 device 块）
static const char DISCOVERY_TAIL[] PROGMEM = "}";

// PROGMEM 串长度（本 core 没有 strlen_P）
static size_t pgmStrLen(const char* s) {
    size_t n = 0;
    while ((char)pgm_read_byte(s + n) != '\0') {
        n++;
    }
    return n;
}

// 把 PROGMEM 串追加到栈缓冲（逐字节、带边界检查，绝不越界）
static void appendPgmStr(char* dst, size_t dstSize, const char* src) {
    size_t pos = strlen(dst);
    while (pos + 1 < dstSize) {
        char c = (char)pgm_read_byte(src);
        if (c == '\0') break;
        dst[pos++] = c;
        src++;
    }
    dst[pos] = '\0';
}

// 把 RAM 串追加到栈缓冲
static void appendRamStr(char* dst, size_t dstSize, const char* src) {
    size_t pos = strlen(dst);
    while (pos + 1 < dstSize && *src != '\0') {
        dst[pos++] = *src++;
    }
    dst[pos] = '\0';
}

HAMQTTHandler::HAMQTTHandler(MQTTClient& mqtt, IRMapper& mapper,
                             IRForwarder& forwarder, Logger& logger)
    : mqtt(mqtt), mapper(mapper), forwarder(forwarder), logger(logger), 
      learner(nullptr), discoveryPublished(false), 
      lastDiscoveryAttempt(0), firstConnectedTime(0), discoveryRetryCount(0),
      lastStatePublishTime(0), lastStateSyncCheckTime(0), lastPublishedForwardingEnabled(false),
      lastPublishedRuleCount(0), stateCacheInitialized(false) {
    // 初始化状态缓存数组
    lastPublishedStatus[0] = '\0';
    lastPublishedLearnMode[0] = '\0';
    instance = this;
}

bool HAMQTTHandler::init() {
    mqtt.setCallback(mqttCallback);
    
    // 设置日志推送回调（事件驱动模式）
    logger.setLogPublishCallback([](const char* type, const char* content) {
        if (instance != nullptr) {
            // 格式化日志条目（与HA传感器格式一致）
            char logStr[128];
            snprintf(logStr, sizeof(logStr), "[%s] %s", type, content);
            instance->publishLog(String(logStr));
        }
    });
    
    // 订阅将在 MQTT 连接成功后进行
    return true;
}

void HAMQTTHandler::update() {
    mqtt.update();
    
    // 当 MQTT 连接成功时订阅命令主题
    // ⚠️ 这里以前只看"连接状态跳变"（wasConnected），而订阅前的 checkMemory(2048) 是软门槛：
    // 只要那一次恰好内存不足，就会**跳过订阅**，但 wasConnected 仍被无条件置 true ⇒
    // 本轮不再重试 ⇒ 设备"连着 MQTT 却完全听不到命令"（实测撞到过一次）。
    // 现在把"是否已订阅成功"单独记一个状态，没订上就每 2s 重试一次。
    static bool wasConnected = false;
    static unsigned long lastSubscribeAttempt = 0;
    bool isConnected = mqtt.isConnected();
    
    if (isConnected) {
        unsigned long nowSub = millis();
        if (nowSub - lastSubscribeAttempt >= 2000) {
            lastSubscribeAttempt = nowSub;
            if (!hasPendingSubscription()) {
                // 该订的都订上了（或已试到上限）。以前这里没有这个出口 ⇒
                // 只要订阅"没完全成功"，每 2 秒就会重来一遍，一辈子不停。
            } else if (MemoryMonitor::checkMemory(2048)) {
                for (size_t i = 0; i < SUB_SLOT_COUNT; i++) {
                    MqttSubSlot& slot = subSlots[i];
                    if (slot.ok) {
                        continue;
                    }
                    if (slot.attempts >= slot.maxAttempts) {
                        continue;   // 这个主题已经试到上限，放弃
                    }
                    slot.attempts++;
                    ESP.wdtFeed();   // 喂看门狗，避免订阅过程中超时
                    bool r = mqtt.subscribe(slot.topic);
                    slot.ok = r;
                    // ⚠️ 这些必须是**常开**的输出。原来全是 DEBUG_PRINT*，而它在未开
                    // DEBUG_ENABLED 时被编译期删掉 ⇒ "订阅到底订上了没有"这个最关键的
                    // 事实在设备上完全不可观测。
                    if (r) {
                        Serial.printf_P(PSTR("[HA MQTT] 已订阅: %s\n"), slot.topic);
                    } else {
                        Serial.printf_P(PSTR("[HA MQTT] 订阅失败(第 %u/%u 次): %s\n"),
                                        (unsigned)slot.attempts, (unsigned)slot.maxAttempts, slot.topic);
                    }
                }
                ESP.wdtFeed();   // 订阅完成后再次喂看门狗
            } else {
                Serial.printf_P(PSTR("[HA MQTT] 内存不足，稍后重试订阅 (空闲: %d bytes)\n"), ESP.getFreeHeap());
            }
        }
        wasConnected = true;
    } else if (!isConnected && wasConnected) {
        // 连接断开
        Serial.println(F("[HA MQTT] MQTT 连接已断开"));
        wasConnected = false;
        discoveryPublished = false;
        lastDiscoveryAttempt = 0;
        firstConnectedTime = 0;
        discoveryRetryCount = 0;
        resetSubscriptions();   // 重连后要重新订一遍
        // 清掉状态缓存：重连（或 HA 侧刚重启）后强制完整重发一次 state，
        // 否则 HA 可能一直停在"上一次发布"的陈旧值 / unknown。
        stateCacheInitialized = false;
    }
    
    // 当 MQTT 连接成功且未发布 Discovery 时，发布配置
    // 使用递增重试间隔：5秒、10秒、20秒，避免频繁重试占用资源
    if (isConnected && !discoveryPublished) {
        unsigned long now = millis();
        
        // 确保连接稳定后再发布（连接后至少等待 1 秒）
        if (firstConnectedTime == 0) {
            firstConnectedTime = now;
            discoveryRetryCount = 0;
        }
        
        if (now - firstConnectedTime >= 1000) {
            // 计算重试间隔：第1次5秒，第2次10秒，第3次及以后20秒
            unsigned long retryInterval = 5000;  // 默认5秒
            if (discoveryRetryCount == 1) {
                retryInterval = 10000;  // 第2次10秒
            } else if (discoveryRetryCount >= 2) {
                retryInterval = 20000;  // 第3次及以后20秒
            }
            
            if (now - lastDiscoveryAttempt >= retryInterval) {
                lastDiscoveryAttempt = now;
                discoveryRetryCount++;
                publishDiscoveryConfig();
            }
        }
    } else if (!isConnected) {
        // 连接断开时，重置重试计数
        firstConnectedTime = 0;
        discoveryRetryCount = 0;
    }
    
    // ── 状态发布 / 定期回推（v1.3.17）────────────────────────────────
    // 这里原先挂着两个条件：首次连接发一次状态、以及每 5s 空转回推一次 —— 但两者都在
    // `isConnected` 之外还要求 `discoveryPublished`。那是个错误依赖：只要 Discovery 因为
    // 任何原因没发出去（本机就是这种情况：配了 MQTT 后空闲堆 ~4.0 KB < 4096 门槛），
    // 状态就永远不发，HA 里永远停在陈旧值。状态发布只该依赖"MQTT 连着"。
    //
    // publishEnhancedState() 内部有状态缓存：无变化时在**分配 JSON 之前**就 return，
    // 空转成本只是几次字段比较；一旦检出变化（无论改动来自 Web / 按钮 / 串口 / MQTT）
    // 就照常发布。断线时缓存被清 ⇒ 重连后强制完整重发一次。
    if (isConnected) {
        unsigned long nowSync = millis();
        if (!stateCacheInitialized || (nowSync - lastStateSyncCheckTime >= STATE_SYNC_INTERVAL_MS)) {
            lastStateSyncCheckTime = nowSync;
            publishEnhancedState();
        }
    }
}

void HAMQTTHandler::handleCommand(const String& topic, const String& payload) {
    // 检查是否是 SmartIR 主题（直接发送 Base64 字符串）
    if (topic.startsWith("smartir/") && topic.endsWith("/send")) {
        // SmartIR 直接发送 Base64 字符串，不需要 JSON 包装
        handleSmartIRSignal(payload);
        return;
    }
    
    // 处理设备命令主题
    if (topic == MQTT_TOPIC_COMMAND) {
        processCommand(payload);
    }
}

void HAMQTTHandler::handleCommand(const char* topic, const char* payload, size_t payloadLen) {
    if (topic == nullptr || payload == nullptr) {
        return;
    }

    // MQTT 报文不保证以 '\0' 结尾，因此按**长度**拼 String（不再另开常驻缓冲）
    // 上限沿用原实现的 1024 B 截断，避免超大报文吃掉本就紧张的堆
    if (payloadLen > MAX_MQTT_PAYLOAD_LEN) {
        payloadLen = MAX_MQTT_PAYLOAD_LEN;
    }
    String payloadStr;
    payloadStr.concat(payload, (unsigned int)payloadLen);

    // 检查是否是 SmartIR 主题（直接发送 Base64 字符串）
    if (strncmp(topic, "smartir/", 8) == 0 && strstr(topic, "/send") != nullptr) {
        // SmartIR 直接发送 Base64 字符串，不需要 JSON 包装
        handleSmartIRSignal(payloadStr);
        return;
    }
    
    // 处理设备命令主题
    if (strcmp(topic, MQTT_TOPIC_COMMAND) == 0) {
        processCommand(payloadStr);
    }
}

// APIResponseHandler 接口实现
bool HAMQTTHandler::sendSuccess(const String& json, const char* context) {
    if (!mqtt.isConnected()) {
        char errorMsg[128];
        snprintf(errorMsg, sizeof(errorMsg), "[MQTT%s] MQTT not connected, failed to publish", context ? context : "");
        logger.log("error", errorMsg);
        Serial.printf_P(PSTR("[MQTT%s] MQTT not connected, failed to publish\n"), context ? context : "");
        return false;
    }

    // MQTT state 主题统一结构：
    // - data 始终为对象
    // - data.forwardingEnabled 始终存在
    // - 若原 data 不是对象，则放入 data.result
    //
    // ⚠️ 字段名与 HTTP /api/status 保持一致用 camelCase（v1.3.19 统一）。
    // 下面的"快速判断"是**按字符串**找这个键的，所以它与键名是一体的：
    // 改键名而漏改这里的字面量，归一化就会以为"没有这个字段"、再注入一份旧名，
    // 于是 payload 里同时出现两个键，HA 的模板读到哪个全看顺序。
    String publishJson = json;
    const bool forwardingEnabled = forwarder.isEnabled();

    // 快速判断：如果已包含 forwardingEnabled 或 data 是数组，则跳过规范化
    // 优化：避免不必要的JSON解析，节省内存和CPU
    bool needsNormalization = true;
    if (publishJson.indexOf("\"forwardingEnabled\"") >= 0) {
        // 已包含 forwardingEnabled，无需规范化
        needsNormalization = false;
    } else if (publishJson.indexOf("\"data\":[") >= 0 || publishJson.indexOf("\"data\" : [") >= 0) {
        // data 是数组（如 query_maps），无需规范化
        needsNormalization = false;
    }

    // 仅在需要时解析并规范化（兼顾内存）
    size_t freeHeap = ESP.getFreeHeap();
    if (needsNormalization) {
        // 规范化会产生额外内存开销（解析 + 可能复制 data），预留更多空间
        size_t docSize = (publishJson.length() * 4) + 1024;
        if (docSize < 2048) docSize = 2048;
        if (docSize > 12288) docSize = 12288;

        // 规范化：确保 data 是对象且包含 forwardingEnabled
        if (freeHeap >= docSize + 2048) {
            DynamicJsonDocument doc(docSize);
            DeserializationError err = deserializeJson(doc, publishJson);
            
            if (!err && doc.is<JsonObject>()) {
                JsonObject root = doc.as<JsonObject>();
                JsonVariant dataVar = root["data"];

                if (!dataVar || dataVar.isNull()) {
                    // data 不存在，创建新对象
                    JsonObject dataObj = root.createNestedObject("data");
                    dataObj["forwardingEnabled"] = forwardingEnabled;
                } else if (dataVar.is<JsonObject>()) {
                    // data 是对象，直接添加 forwardingEnabled
                    JsonObject dataObj = dataVar.as<JsonObject>();
                    dataObj["forwardingEnabled"] = forwardingEnabled;
                } else {
                    // data 不是对象（可能是数组或字符串），包装为对象
                    root["_result_tmp"] = dataVar;
                    JsonObject dataObj = root["data"].to<JsonObject>();
                    dataObj["forwardingEnabled"] = forwardingEnabled;
                    dataObj["result"] = root["_result_tmp"];
                    root.remove("_result_tmp");
                }

                // 序列化规范化后的 JSON
                String normalized;
                serializeJson(doc, normalized);
                if (normalized.length() > 0 && !doc.overflowed()) {
                    publishJson = normalized;
                }
            }
            // 如果解析或序列化失败，保持原样发布
        }
        // 如果内存不足，保持原样发布
    }

    // 检查消息大小（以最终发布的 payload 为准）
    size_t topicLen = strlen(MQTT_TOPIC_STATE);
    size_t payloadLen = publishJson.length();
    size_t messageSize = topicLen + payloadLen;
    
    // 如果消息接近缓冲区大小，记录警告
    if (messageSize > MQTT_BUFFER_SIZE * 0.9 && messageSize <= MQTT_BUFFER_SIZE) {
        char warnMsg[256];
        snprintf(warnMsg, sizeof(warnMsg), "[MQTT%s] Message size warning: %d bytes (%.1f%% of buffer, max: %d bytes)", 
                 context ? context : "", messageSize, (messageSize * 100.0f / MQTT_BUFFER_SIZE), MQTT_BUFFER_SIZE);
        logger.log("warning", warnMsg);
    }
    
    if (messageSize > MQTT_BUFFER_SIZE) {
        char errorMsg[256];
        snprintf(errorMsg, sizeof(errorMsg), "[MQTT%s] Message too large: %d bytes (topic: %d, payload: %d, max: %d bytes)", 
                 context ? context : "", messageSize, topicLen, payloadLen, MQTT_BUFFER_SIZE);
        logger.log("error", errorMsg);
        Serial.printf_P(PSTR("[MQTT%s] Message too large: %d bytes (topic: %d, payload: %d, max: %d bytes)\n"), 
                      context ? context : "", messageSize, topicLen, payloadLen, MQTT_BUFFER_SIZE);
        // 尝试按原样发布（若原消息更小）
        // 注意：对于 query_maps 等数据查询接口，不要发布状态信息，避免覆盖数据
        size_t originalPayloadLen = json.length();
        size_t originalMessageSize = topicLen + originalPayloadLen;
        if (originalMessageSize <= MQTT_BUFFER_SIZE) {
            char fallbackMsg[192];
            snprintf(fallbackMsg, sizeof(fallbackMsg), "[MQTT%s] Using original message (size: %d bytes)", 
                     context ? context : "", originalMessageSize);
            logger.log("warning", fallbackMsg);
            bool fallbackResult = mqtt.publish(MQTT_TOPIC_STATE, json);
            publishEnhancedState();
            return fallbackResult;
        }
        // 如果原消息也太大，记录详细信息
        char detailMsg[256];
        snprintf(detailMsg, sizeof(detailMsg), "[MQTT%s] Original message also too large: %d bytes (payload: %d)", 
                 context ? context : "", originalMessageSize, originalPayloadLen);
        logger.log("error", detailMsg);
        return false;
    }
    
    // 检查内存
    if (freeHeap < messageSize + 512) {
        char errorMsg[192];
        snprintf(errorMsg, sizeof(errorMsg), "[MQTT%s] Insufficient memory: %d bytes free (need: %d bytes)", 
                 context ? context : "", freeHeap, messageSize + 512);
        logger.log("error", errorMsg);
        Serial.printf_P(PSTR("[MQTT%s] Insufficient memory: %d bytes free (need: %d bytes)\n"), 
                      context ? context : "", freeHeap, messageSize + 512);
        return false;
    }
    
    bool result = mqtt.publish(MQTT_TOPIC_STATE, publishJson);
    if (!result) {
        char errorMsg[128];
        snprintf(errorMsg, sizeof(errorMsg), "[MQTT%s] Failed to publish message", context ? context : "");
        logger.log("error", errorMsg);
        Serial.printf_P(PSTR("[MQTT%s] Failed to publish message\n"), context ? context : "");
    }
    return result;
}

bool HAMQTTHandler::sendError(const String& error, int code, const char* context) {
    // MQTT 不使用 HTTP 状态码，code 参数忽略
    String output = UnifiedAPI::createErrorResponse(error, code);
    return sendSuccess(output, context);  // 使用 sendSuccess 发送错误响应（已包含错误格式）
}

void HAMQTTHandler::publishState(const String& state, const String& message) {
    // 优化：避免在MQTT回调中直接调用publishEnhancedState()，可能导致栈溢出
    // 如果内存充足，直接发布简单状态；否则延迟到下次update()
    if (MemoryMonitor::checkMemory(2048) && mqtt.isConnected()) {
        // 使用统一格式
        String output;
        if (state == "success") {
            output = UnifiedAPI::createSuccessResponse(message, "success");
        } else if (state == "error") {
            output = UnifiedAPI::createErrorResponse(message);
        } else if (state == "executing") {
            output = UnifiedAPI::createExecutingResponse(message);
        } else {
            // 兼容旧格式，但使用统一结构
            DynamicJsonDocument doc(256);
            doc["success"] = true;
            doc["status"] = state;
            doc["data"] = message;
            UnifiedAPI::addTimestamp(doc);
            serializeJson(doc, output);
        }
        sendSuccess(output, nullptr);  // 使用统一的发送方法，包含错误检查
    }
    // 如果内存不足，跳过发布（状态将在下次update()时通过publishEnhancedState()发布）
}

void HAMQTTHandler::publishLog(const String& logEntry) {
    // 优化：检查内存和连接状态
    // ⚠️ 这里以前还要求 `discoveryPublished`。那是个错误依赖：Discovery 只是因为
    // 当时内存不够没发出去时（本机常态），日志推送也一起被掐掉 —— 而"日志收不到"
    // 恰恰是最需要它的时候。推日志只该依赖"MQTT 连着 + 内存够"。
    if (!mqtt.isConnected()) {
        return;
    }
    
    // 内存检查：日志推送需要较少内存（约512字节）
    if (!MemoryMonitor::checkMemory(512)) {
        return;  // 内存不足，跳过推送
    }
    
    // 发布到日志主题，更新 Home Assistant 日志传感器状态
    mqtt.publish(MQTT_TOPIC_LOG, logEntry);
}

void HAMQTTHandler::publishDiscoveryConfig() {
    if (discoveryPublished) return;
    
    if (!mqtt.isConnected()) {
        DEBUG_PRINTLN("[HA Discovery] MQTT 未连接，无法发布 Discovery 配置");
        return;
    }
    
    // v1.3.17：整段已改为**零堆分配**（PROGMEM 模板 + 栈缓冲），
    // 因此不再需要原来那句 `freeHeap < 4096 ⇒ 跳过` —— 它正是本机"配了 MQTT 就永远
    // 发不出 Discovery"的原因（配 MQTT 后空闲堆只有 ~4.0 KB，门槛贴得比实机水位还高）。
    // 只留一条最低安全线，真正的失败由 mqtt.publish 自己兜底。
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 1024) {
        DEBUG_PRINTF("[HA Discovery] 内存过低，跳过发布 (空闲: %d bytes)\n", freeHeap);
        return;
    }

    DEBUG_PRINTLN("[HA Discovery] 开始发布 Home Assistant Discovery 配置...");
    DEBUG_PRINTF("[HA Discovery] MQTT 缓冲区大小: %d bytes, 空闲内存: %d bytes\n",
                  MQTT_BUFFER_SIZE, freeHeap);

    // 喂看门狗，避免发布过程中超时
    ESP.wdtFeed();

    char chipIdStr[16];
    snprintf(chipIdStr, sizeof(chipIdStr), "%u", ESP.getChipId());

    // 4 条配置共用的 device 块（HA 靠它把实体归到同一个设备下）
    char deviceJson[224];
    deviceJson[0] = '\0';
    appendPgmStr(deviceJson, sizeof(deviceJson), DISCOVERY_DEVICE_HEAD);
    appendRamStr(deviceJson, sizeof(deviceJson), chipIdStr);
    appendPgmStr(deviceJson, sizeof(deviceJson), DISCOVERY_DEVICE_TAIL);

    bool allPublished = true;

    if (!publishDiscoveryEntity("binary_sensor", "status", DISCOVERY_STATUS_HEAD, deviceJson)) {
        allPublished = false;
    }
    if (!publishDiscoveryEntity("sensor", "state", DISCOVERY_STATE_HEAD, deviceJson)) {
        allPublished = false;
    }
    if (!publishDiscoveryEntity("sensor", "log", DISCOVERY_LOG_HEAD, deviceJson)) {
        allPublished = false;
    }
    if (!publishDiscoveryEntity("switch", "forwarding_enabled", DISCOVERY_SWITCH_HEAD, deviceJson)) {
        allPublished = false;
    }

    // 日志传感器的初始状态：沿用最近一条日志，避免 HA 里显示 unknown
    if (allPublished) {
        char initialLog[128] = "无日志";
        {
            // 数组用完即出作用域，栈帧可被后续复用（LogEntry × 10 ≈ 880 B）
            LogEntry entries[MAX_LOG_ENTRIES];
            int logCount = logger.getLogCount();
            if (logCount > 0 && logger.getLogs(entries, logCount)) {
                snprintf(initialLog, sizeof(initialLog), "[%s] %s",
                         entries[logCount - 1].type, entries[logCount - 1].content);
            }
        }
        if (mqtt.publish(MQTT_TOPIC_LOG, initialLog, true)) {
            DEBUG_PRINTLN("[HA Discovery] ✓ 日志传感器初始状态已发布");
        }
    }

    if (allPublished) {
        discoveryPublished = true;
        DEBUG_PRINTLN("[HA Discovery] ✓ 所有 Discovery 配置已成功发布");
        logger.log("mqtt", "Home Assistant Discovery 配置已发布");

        // 发布在线状态（与 LWT 主题一致，保留消息）
        mqtt.publish(MQTT_TOPIC_STATE "/status", "online", true);

        // Discovery 发布成功后，发布初始设备状态（事件驱动）
        publishEnhancedState();
    } else {
        DEBUG_PRINTLN("[HA Discovery] ✗ 部分配置发布失败，将在下次重试");
    }
}

// 发布一条 Discovery 实体配置：topic 拼进栈缓冲，
// payload = 头模板(PROGMEM) + device 块 + "}"，全程不碰堆。
// 这正是「配了 MQTT（空闲堆 ~4 KB）也能发出 Discovery」的前提。
bool HAMQTTHandler::publishDiscoveryEntity(const char* entityClass, const char* entityId,
                                           const char* configHead, const char* deviceJson) {
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config",
             MQTT_DISCOVERY_PREFIX, entityClass, MQTT_DEVICE_ID, entityId);

    // 768 B 覆盖最长的一条（forwarding_enabled 开关约 635 B）+ chipId 余量
    char payload[768];
    size_t need = pgmStrLen(configHead) + strlen(deviceJson) + pgmStrLen(DISCOVERY_TAIL) + 1;
    if (need > sizeof(payload)) {
        DEBUG_PRINTF("[HA Discovery] ✗ %s 配置超长（%u bytes），跳过\n", entityId, (unsigned)need);
        return false;
    }

    payload[0] = '\0';
    appendPgmStr(payload, sizeof(payload), configHead);
    appendRamStr(payload, sizeof(payload), deviceJson);
    appendPgmStr(payload, sizeof(payload), DISCOVERY_TAIL);

    ESP.wdtFeed();
    if (mqtt.publish(topic, payload, true)) {
        DEBUG_PRINTF("[HA Discovery] ✓ %s 配置已发布 (%u bytes)\n",
                     entityId, (unsigned)strlen(payload));
        return true;
    }
    DEBUG_PRINTF("[HA Discovery] ✗ %s 配置发布失败\n", entityId);
    return false;
}

void HAMQTTHandler::processCommand(const String& payload) {
    // 长度闸：阈值与收发缓冲同源，推导见 constants.h 的 MQTT_MAX_INBOUND_PAYLOAD。
    // 放在最前面 —— 超长 payload 连 JSON 解析都不必做。
    if (payload.length() > MQTT_MAX_INBOUND_PAYLOAD) {
        sendError("Payload too long: " + String(payload.length()) + " > " +
                  String(MQTT_MAX_INBOUND_PAYLOAD), 413, "MQTT: processCommand");
        return;
    }

    // 使用统一的内存检查
    if (!MemoryMonitor::checkMemory(2048)) {
        DEBUG_PRINTLN("[HA MQTT] 内存不足，跳过命令处理");
        return;
    }
    
    // 检查是否是转发开关的命令（直接发送 true/false）
    String trimmedPayload = payload;
    trimmedPayload.trim();
    if (trimmedPayload == "true" || trimmedPayload == "false") {
        // 包装成JSON格式调用 handleSetForwardingEnabled()
        DynamicJsonDocument doc(128);
        doc["enabled"] = (trimmedPayload == "true");
        String jsonPayload;
        serializeJson(doc, jsonPayload);
        handleSetForwardingEnabled(jsonPayload);
        return;
    }
    
    // 优化：根据payload长度动态计算JSON大小，加强内存检查
    size_t jsonSize = payload.length() + 512;
    if (jsonSize > 3072) jsonSize = 3072; // 限制最大3KB
    if (jsonSize < 512) jsonSize = 512;
    
    // 加强内存检查：确保有足够的内存（JSON大小 + 安全缓冲）
    if (!MemoryMonitor::checkMemory(jsonSize + 1024)) {
        DEBUG_PRINTLN("[HA MQTT] 内存不足，跳过命令处理");
        return;
    }
    
    // ⚠️ 解析用的 doc 必须**在分发前销毁**（v1.3.17）：
    // 它以前一直活到函数返回，于是 handleUpdateMap / handleForwardTest 等处理函数执行时，
    // 堆上还压着这 0.5~3 KB —— 配了 MQTT 后空闲堆只有 ~4 KB，本次实测的
    // "update_map → Low Memory" 就是这么来的。各处理函数用的都是**原始 payload**，
    // 没有一个依赖这个 doc，所以把 action 拷出来、立刻还回去即可。
    // （用作用域析构而不是 doc.clear() —— clear() 只清内容，内存池仍然占着。）
    String action;
    String parseError;
    {
        DynamicJsonDocument doc(jsonSize);
        DeserializationError error = deserializeJson(doc, payload);
        if (error) {
            char errorMsg[64];
            snprintf(errorMsg, sizeof(errorMsg), "Invalid JSON: %s", error.c_str());
            parseError = errorMsg;
        } else {
            action = doc["action"] | "";
        }
    }

    if (parseError.length() > 0) {
        publishState("error", parseError);
        return;
    }

    // ── action 分派（v1.3.22 裁剪）───────────────────────────────────────
    // 唯一的判据：**这个能力 Web 面板有没有替代**。面板自 v1.2.0 起常驻且免填 IP，
    // 重操作没必要再挤 MQTT 这条 3.6 KB 空闲堆的窄路。
    // 裁掉它们的直接收益：唯一撑起 8 KB 收发缓冲的大报文全部消失
    // （`query_maps` 回完整映射表 ~6.7 KB、`update_map` 入站可含整批规则、
    // `get_forward_records` 出站 1034 B）⇒ 缓冲可以降到 1024 B（constants.h:182）。
    // 被裁的 action 在此**显式拒绝**，不落进 "Unknown action" —— 否则以后
    // 有人调用会以为是名字拼错，而不是"这个入口已经关了"。
    if (action == "get_status") {
        handleGetStatus();
    } else if (action == "get_logs") {
        handleGetLogs();
    } else if (action == "set_forwarding_enabled") {
        // ⚠️ 这个 action 名**不跟着**字段命名统一（它保持 snake）：
        // 它是 HA → 设备方向的**命令动词**，不是状态字段。手写自动化里会直接写
        // {"action":"set_forwarding_enabled"}，改名等于让它们静默变成 "Unknown action"。
        // 字段命名统一只覆盖"设备 → HA 的 payload 键"。
        handleSetForwardingEnabled(payload);
    } else if (action == "send_signal") {
        // 保留：与 SmartIR 走同一套解析（parseAndSendSignal），不额外抬高缓冲下界。
        handleSendSignal(payload);
    } else if (action == "query_maps" || action == "update_map" ||
               action == "start_learn" || action == "stop_learn" ||
               action == "query_learn_status" || action == "query_learn_result" ||
               action == "get_recent_signals" || action == "get_forward_records" ||
               action == "forward_test") {
        sendError("Action '" + action + "' was removed from MQTT (use the web panel)",
                  400, "MQTT: processCommand");
    } else {
        sendError("Unknown action: " + action, 400, "MQTT: processCommand");
    }
}

void HAMQTTHandler::handleQueryMaps() {
    // 获取映射表数据
    String dataJson = APIHandlers::handleQueryMaps(mapper, &logger);
    
    if (dataJson.length() == 0) {
        logger.log("error", "[MQTT: query_maps] handleQueryMaps returned empty string");
        sendError("Failed to get mapping rules", 500, "MQTT: query_maps");
        return;
    }
    
    // 构建响应：将数组包装为对象，添加 forwardingEnabled 字段
    // 格式：{"success":true,"status":"success","data":{"forwardingEnabled":true,"rules":[...]},"timestamp":...}
    bool forwardingEnabled = forwarder.isEnabled();
    String output;
    output.reserve(dataJson.length() + 300);  // 预分配内存
    
    output = "{\"success\":true,\"status\":\"success\",\"data\":{\"forwardingEnabled\":";
    output += forwardingEnabled ? "true" : "false";
    output += ",\"rules\":";
    output += dataJson;
    output += "},\"timestamp\":";
    output += String(millis());
    output += "}";
    
    // 直接发布，避免 sendSuccess() 的规范化逻辑
    if (!mqtt.isConnected()) {
        logger.log("error", "[MQTT: query_maps] MQTT not connected");
        return;
    }
    
    size_t messageSize = strlen(MQTT_TOPIC_STATE) + output.length();
    if (messageSize > MQTT_BUFFER_SIZE) {
        logger.log("error", "[MQTT: query_maps] Message too large");
        return;
    }
    
    if (!mqtt.publish(MQTT_TOPIC_STATE, output)) {
        logger.log("error", "[MQTT: query_maps] Failed to publish message");
    }
}

void HAMQTTHandler::handleUpdateMap(const String& json) {
    // 使用公共函数
    bool success = false;
    String message = APIHandlers::handleUpdateMap(mapper, json, success);
    
    if (success) {
        publishState("success", message);
        publishEnhancedState();  // 规则数量可能变化，发布设备状态
    } else {
        publishState("error", message);
    }
}

void HAMQTTHandler::handleGetLogs() {
    // 使用公共函数（返回全部日志，环形缓冲最多 MAX_LOG_ENTRIES = 10 条）
    String dataJson = APIHandlers::handleGetLogs(logger);
    
    // 使用统一的响应处理
    processJsonArrayResponse(dataJson, "MQTT: get_logs", 4096);
}

void HAMQTTHandler::handleStartLearn(const String& json) {
    if (learner == nullptr) {
        publishState("error", "Learner service not initialized");
        return;
    }
    
    // 使用公共函数
    bool success = false;
    String message = APIHandlers::handleStartLearn(*learner, json, success);
    
    if (success) {
        publishState("success", message);
        publishEnhancedState();  // 学习状态变化，发布设备状态
    } else {
        publishState("error", message);
    }
}

void HAMQTTHandler::handleQueryLearnStatus(const String& json) {
    if (learner == nullptr) {
        sendError("Service not initialized", 500, "MQTT: query_learn_status");
        return;
    }
    
    // 使用公共函数（简化版）
    String dataJson = APIHandlers::handleQueryLearnStatus(*learner);
    
    // 使用统一的响应处理
    processJsonObjectResponse(dataJson, "MQTT: query_learn_status", 1024);
}

void HAMQTTHandler::handleStopLearn(const String& json) {
    if (learner == nullptr) {
        publishState("error", "Learner service not initialized");
        return;
    }
    
    // 使用公共函数
    bool success = false;
    String message = APIHandlers::handleStopLearn(*learner, success);
    
    if (success) {
        publishState("success", message);
        publishEnhancedState();  // 学习状态变化，发布设备状态
    } else {
        publishState("error", message);
    }
}

void HAMQTTHandler::handleQueryLearnResult(const String& json) {
    if (learner == nullptr) {
        sendError("Service not initialized", 500, "MQTT: query_learn_result");
        return;
    }
    
    // 解析format参数（从JSON中读取）
    String format = "base64";  // 默认格式
    if (json.length() > 0) {
        size_t jsonSize = json.length() + 256;
        if (jsonSize > 512) jsonSize = 512;
        DynamicJsonDocument doc(jsonSize);
        DeserializationError err = deserializeJson(doc, json);
        if (!err && doc.containsKey("format")) {
            format = doc["format"] | "base64";
        }
    }
    
    // 使用公共函数
    APIHandlers::LearnQueryStatus st = APIHandlers::LearnQueryStatus::OK;
    String dataJson = APIHandlers::handleQueryLearnResult(*learner, format, &st);
    if (dataJson.length() == 0) {
        // 与 HTTP 侧同一个判据：三种失败给三种说明，别再一律报"没学过"
        switch (st) {
            case APIHandlers::LearnQueryStatus::NO_DATA:
                sendError("No learned device found", 404, "MQTT: query_learn_result");
                break;
            case APIHandlers::LearnQueryStatus::LOW_MEMORY:
                sendError("Insufficient memory", 503, "MQTT: query_learn_result");
                break;
            default:
                sendError("Failed to convert learned result", 500, "MQTT: query_learn_result");
                break;
        }
        return;
    }
    
    // ⚠️⚠️ 与 HTTP 侧同一处修正（v1.3.28）：这里原来也有一块
    //   `DynamicJsonDocument dataDoc(4096)`，只为把 dataJson 内联进 data。
    //   而"刚学完"正是内存最低的时刻（实测 maxBlock 2792 B）⇒ 必然分配失败
    //   ⇒ data 忽而是对象、忽而是字符串；前端/HA 按对象取就恒取不到。
    //   现在统一按字符串返回（createSuccessResponse 的容量是 256 + 内容长度，很省）。
    String output = UnifiedAPI::createSuccessResponse(dataJson, "success", &logger, "MQTT: query_learn_result");
    sendSuccess(output, "MQTT: query_learn_result");
}

void HAMQTTHandler::handleGetRecentSignals(const String& json) {
    // 使用公共函数
    String dataJson = APIHandlers::handleGetRecentSignals(logger);
    
    // 使用统一的响应处理
    processJsonArrayResponse(dataJson, "MQTT: get_recent_signals", 2048);
}

// 转发记录：HA 侧不开网页也能查"刚才那条信号到底转发没有、转到哪了"
void HAMQTTHandler::handleGetForwardRecords() {
    // 使用公共函数（与 HTTP GET /api/forward/records 同一份实现）
    String dataJson = APIHandlers::handleGetForwardRecords(forwarder);

    // 使用统一的响应处理
    processJsonArrayResponse(dataJson, "MQTT: get_forward_records", 2048);
}

// 试跑一条 A 信号：立刻匹配并发射，把结论直接说清楚
// payload 形如 {"action":"forward_test","a":"1,0x11B08F7"}（也接受纯字符串）
void HAMQTTHandler::handleForwardTest(const String& json) {
    if (!MemoryMonitor::checkMemory(2048)) {
        DEBUG_PRINTLN("[HA MQTT] 内存不足，跳过试跑");
        return;
    }

    bool success = false;
    String message = APIHandlers::handleForwardTest(forwarder, json, success);

    // message 已是人可读结论（已匹配规则并发出 / 没有任何规则匹配这个信号 / ...）
    publishState(success ? "success" : "error", message);
}

void HAMQTTHandler::handleSendSignal(const String& payload) {
    // ⚠️ 以前这里自己写了一套 3 分支的 JSON 解包（其中一段还永远走不到），
    // 而 HTTP 侧的同名动作只有 trim + parse ⇒ 同一个 JSON 在网页上发得出去、
    // 在 MQTT 上发不出去，且两边报错口径也不一样（这边永远只说
    // "Failed to send signal"，连"内存不足"都被吞掉）。
    // 现在统一走 APIHandlers：解包逻辑一份，错误文案一份。
    bool ok = false;
    String message = APIHandlers::handleSendSignal(forwarder, payload, ok);
    if (ok) {
        publishState("executing", "Signal sent");
    } else {
        publishState("error", message);
    }
}

void HAMQTTHandler::handleSetForwardingEnabled(const String& json) {
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, json);
    
    if (error) {
        publishState("error", "Invalid JSON");
        return;
    }
    
    // ⚠️ 以前是 `doc["enabled"] | true`：字段缺失或写错类型时**默认把转发打开**。
    // 这是一个写操作，缺参数就该当错误处理 —— 猜错的那个方向恰好是"让红外发出去"。
    if (!doc.containsKey("enabled") || !doc["enabled"].is<bool>()) {
        sendError("Missing 'enabled' field (true/false)", 400, "MQTT: set_forwarding_enabled");
        return;
    }
    bool enabled = doc["enabled"].as<bool>();
    forwarder.setEnabled(enabled);
    // 落盘：MQTT 与网页两个通道改的是同一个开关，重启后行为必须一致
    DeviceState::setForwardingEnabled(enabled);
    
    // 使用统一格式
    String message = "Forwarding " + String(enabled ? "enabled" : "disabled");
    String output = UnifiedAPI::createSuccessResponse(message, "success");
    mqtt.publish(MQTT_TOPIC_STATE, output);
    
    // 转发状态变化，发布设备状态
    publishEnhancedState();
}

void HAMQTTHandler::handleSmartIRSignal(const String& payload) {
    // SmartIR 发送的格式（纯字符串）：Base64、数组、协议格式
    // ⚠️ 这一整条路径以前只有 DEBUG_PRINT*。而 DEBUG_PRINT* 在未开 DEBUG_ENABLED
    // 时是**编译期删掉**的 ⇒ 设备上出问题时零线索：SmartIR 发过来一条指令、什么都没
    // 发生、串口一个字都没有。改为常开的 Serial 输出（用 PSTR 放 flash，不占 DRAM）。
    // 长度闸：合法信号最长 base64(200 点 × 2 B) = 536 字符（推导见 constants.h）。
    // 超限直接拒绝并打串口 —— 不这么做的后果是解析器把码**静默截断到 200 点**，
    // 表现是"按了按键但设备只发出去一半波形"，比直接拒绝难查得多。
    if (payload.length() > MQTT_MAX_INBOUND_PAYLOAD) {
        Serial.printf_P(PSTR("[SmartIR] payload 过长 %u > %u，拒绝\n"),
                        (unsigned)payload.length(), (unsigned)MQTT_MAX_INBOUND_PAYLOAD);
        return;
    }

    if (!MemoryMonitor::checkMemory(2048)) {
        Serial.printf_P(PSTR("[SmartIR] 内存不足，跳过信号处理 (空闲 %d)\n"), ESP.getFreeHeap());
        return;
    }

    // ⭐ 总堆下限（v1.3.40）：checkMemory(2048) 的第一道只要求 freeHeap ≥ 2048，
    // 总堆只剩 3.6 KB 时**照样放行** ⇒ 随后 parseAndSendSignal() 里分配失败
    // ⇒ SDK 打堆图（关中断、可到秒级）⇒ 软看门狗硬复位（resetReason=3）。
    // 这条路径以前完全没有它：HTTP/MQTT 的「发送」在 v1.3.39 就加上了，SmartIR（HA 主动下发）
    // 是同一套发射、却漏掉了 —— 而 HA 什么时候下发不由我们决定，属于"没人看着时也会踩"。
    // 判据与用法（含"⛔ 别挪到 sendSequence"的实测依据）见 memory_monitor.h。
    if (!MemoryMonitor::hasSendHeadroom("SmartIR")) {
        return;
    }
    
    if (payload.length() == 0) {
        Serial.println(F("[SmartIR] 收到空的 payload"));
        return;
    }
    
    Serial.printf_P(PSTR("[SmartIR] 收到信号，长度: %u bytes\n"), (unsigned)payload.length());
    
    // 使用统一的解析和发送方法
    if (forwarder.parseAndSendSignal(payload)) {
        Serial.println(F("[SmartIR] 信号发送成功"));
    } else {
        Serial.printf_P(PSTR("[SmartIR] 信号发送失败: %s\n"), payload.c_str());
    }
}

void HAMQTTHandler::handleGetStatus() {
    // 使用统一的内存检查
    if (!MemoryMonitor::checkMemory(2048)) {
        sendError("Insufficient memory", 503, "MQTT: get_status");
        return;
    }
    
    // 喂看门狗，避免发布过程中超时
    ESP.wdtFeed();
    
    // 获取当前状态值（复用 publishEnhancedState() 的逻辑）
    const char* currentStatus = "ready";
    if (learner != nullptr && learner->isLearningDevice()) {
        currentStatus = "learning";
    }
    
    bool currentForwardingEnabled = forwarder.isEnabled();
    
    const char* currentLearnMode = "none";
    if (learner != nullptr && learner->isLearningDevice()) {
        currentLearnMode = "device";
    }
    
    int currentRuleCount = mapper.getRuleCount();
    
    // 使用统一格式，但保留所有HA需要的字段
    DynamicJsonDocument doc(512);
    
    // 统一格式：success和status在顶层
    doc["success"] = true;
    doc["status"] = currentStatus;
    
    // 数据放在data对象中（HA需要的字段）
    JsonObject dataObj = doc.createNestedObject("data");
    dataObj["forwardingEnabled"] = currentForwardingEnabled;
    dataObj["learnMode"] = currentLearnMode;
    
    // 学习状态（优化：避免在栈上创建大的LearnedDevice对象）
    if (learner != nullptr && learner->isLearningDevice()) {
        // 直接获取设备名称字符串，避免复制整个LearnedDevice结构体
        String deviceName = learner->getDeviceName();
        dataObj["learnDevice"] = deviceName;
        deviceName = "";  // 立即释放
        
        String progress = learner->getLearnProgress();
        dataObj["learnProgress"] = progress;
        progress = "";  // 立即释放
    } else {
        dataObj["learnProgress"] = "";
        dataObj["learnDevice"] = "";
    }
    
    // 映射规则数量
    dataObj["ruleCount"] = currentRuleCount;
    
    UnifiedAPI::addTimestamp(doc);
    
    // 喂看门狗，避免序列化过程中超时
    ESP.wdtFeed();
    
    String output;
    serializeJson(doc, output);
    sendSuccess(output, "MQTT: get_status");
    
    // 注意：查询接口不更新状态缓存，因为这不是状态变化事件
    // 发布完成后再次喂看门狗
    ESP.wdtFeed();
}

void HAMQTTHandler::publishEnhancedState() {
    // 使用统一的内存检查
    if (!MemoryMonitor::checkMemory(2048)) {
        return;  // 内存不足，跳过发布
    }
    
    // 喂看门狗，避免发布过程中超时
    ESP.wdtFeed();
    
    // 获取当前状态值
    const char* currentStatus = "ready";
    if (learner != nullptr && learner->isLearningDevice()) {
        currentStatus = "learning";
    }
    
    bool currentForwardingEnabled = forwarder.isEnabled();
    
    const char* currentLearnMode = "none";
    if (learner != nullptr && learner->isLearningDevice()) {
        currentLearnMode = "device";
    }
    
    int currentRuleCount = mapper.getRuleCount();
    
    // 检查状态是否有变化（事件驱动）
    bool stateChanged = false;
    if (!stateCacheInitialized) {
        // 首次发布，立即发布
        stateChanged = true;
    } else {
        // 比较各个状态字段
        if (strcmp(currentStatus, lastPublishedStatus) != 0 ||
            currentForwardingEnabled != lastPublishedForwardingEnabled ||
            strcmp(currentLearnMode, lastPublishedLearnMode) != 0 ||
            currentRuleCount != lastPublishedRuleCount) {
            stateChanged = true;
        }
    }
    
    // 如果状态未变化且已初始化，跳过发布（节约资源）
    if (!stateChanged && stateCacheInitialized) {
        return;
    }
    
    // 使用统一格式，但保留所有HA需要的字段
    DynamicJsonDocument doc(512);
    
    // 统一格式：success和status在顶层
    doc["success"] = true;
    doc["status"] = currentStatus;
    
    // 数据放在data对象中（HA需要的字段）
    JsonObject dataObj = doc.createNestedObject("data");
    dataObj["forwardingEnabled"] = currentForwardingEnabled;
    dataObj["learnMode"] = currentLearnMode;
    
    // 学习状态（优化：避免在栈上创建大的LearnedDevice对象）
    if (learner != nullptr && learner->isLearningDevice()) {
        // 直接获取设备名称字符串，避免复制整个LearnedDevice结构体
        String deviceName = learner->getDeviceName();
        dataObj["learnDevice"] = deviceName;
        deviceName = "";  // 立即释放
        
        String progress = learner->getLearnProgress();
        dataObj["learnProgress"] = progress;
        progress = "";  // 立即释放
    } else {
        dataObj["learnProgress"] = "";
        dataObj["learnDevice"] = "";
    }
    
    // 映射规则数量
    dataObj["ruleCount"] = currentRuleCount;
    
    // 注意：最近信号信息已移除，用户可通过 get_recent_signals API接口查询
    // 移除自动获取以减少内存分配和JSON构建开销，提升转发性能
    
    UnifiedAPI::addTimestamp(doc);
    
    // 喂看门狗，避免序列化过程中超时
    ESP.wdtFeed();
    
    String output;
    serializeJson(doc, output);
    bool published = sendSuccess(output, "MQTT: publishEnhancedState");

    // ⚠️ 只有**发布成功**才推进状态缓存。
    // sendSuccess() 在未连接 / 消息超过缓冲区 / 堆不足 / 底层 publish 失败时
    // 会返回 false；以前完全不看返回值就把缓存更新成"已发布"，而 update() 里
    // 5 秒一次的回推又会因为 `stateChanged == false` 直接 return
    // ⇒ 一次抖动之后，HA 里的 forwardingEnabled / ruleCount / learnMode
    // 会一直停在错值上（几十分钟甚至永久）。
    // 这段代码存在的唯一目的就是"状态一定能同步到 HA"，结果被它自己制造了
    // 最该防的那个场景。
    if (published) {
        strncpy(lastPublishedStatus, currentStatus, sizeof(lastPublishedStatus) - 1);
        lastPublishedStatus[sizeof(lastPublishedStatus) - 1] = '\0';
        lastPublishedForwardingEnabled = currentForwardingEnabled;
        strncpy(lastPublishedLearnMode, currentLearnMode, sizeof(lastPublishedLearnMode) - 1);
        lastPublishedLearnMode[sizeof(lastPublishedLearnMode) - 1] = '\0';
        lastPublishedRuleCount = currentRuleCount;
        lastStatePublishTime = millis();
        stateCacheInitialized = true;
    }
    
    // 发布完成后再次喂看门狗
    ESP.wdtFeed();
}

void HAMQTTHandler::mqttCallback(char* topic, byte* payload, unsigned int length) {
    if (instance == nullptr) return;

    if (topic == nullptr || payload == nullptr) return;

    // 不再把 topic/payload 复制到常驻静态数组（原先各留 128 B + 1024 B 的 .bss）。
    // - topic：PubSubClient 已保证以 '\0' 结尾，可直接用；
    // - payload：不保证结尾，交给 handleCommand 按长度处理。
    // 本机空闲堆只有几 KB，省下的每一字节都是可用堆。
    instance->handleCommand(topic, (const char*)payload, (size_t)length);
}

