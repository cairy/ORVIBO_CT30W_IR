#ifndef CONSTANTS_H
#define CONSTANTS_H

// 缓冲区大小配置（优化：减少内存占用）
#define IR_RX_BUFFER_SIZE_MIN 2048   // 2KB 最小缓冲区
#define IR_RX_BUFFER_SIZE_MAX 4096   // 4KB 最大缓冲区
#define IR_RX_BUFFER_SIZE 2048       // 默认 2KB（从4KB减少到2KB，节省2KB）

// 映射表配置（优化：减少内存占用）
#define MAX_MAP_RULES 20             // 最大映射规则数（从30减少到20，节省约1.3KB）
#define MAX_B_SIGNALS_PER_RULE 5     // 每条规则最多5个连续B信号
#define MAX_SIGNAL_CACHE 5            // 信号缓存数量

// 日志配置（优化：减少内存占用）
#define MAX_LOG_ENTRIES 10            // 最大日志条目数（从20减少到10，节省约850字节）
#define MAX_LOG_TYPE_LEN 16           // 日志类型最大长度
#define MAX_LOG_CONTENT_LEN 64         // 日志内容最大长度

// 学习功能配置（受ESP8266内存限制优化）
#define MAX_LEARN_COMMANDS 5          // 最大学习指令数（受内存限制，每个指令约2.4KB）
#define MAX_LEARN_TIMING_COUNT 200    // 每个指令最大时序点数（受内存限制，从300减少到200）
#define MAX_COMMAND_NAME_LEN 32      // 指令名称最大长度
#define MAX_DEVICE_NAME_LEN 64       // 设备名称最大长度
#define MAX_RECENT_SIGNALS 3         // 最近接收信号记录数（从5减少到3，节省约44字节）
#define DEFAULT_LEARN_TIMEOUT_SEC 30 // 默认学习超时时间（秒）

// 信号处理配置
#define SIGNAL_TIMEOUT_MS 500         // 连续信号识别超时（500ms）
#define SIGNAL_INTERVAL_MIN_MS 0      // 信号间隔最小值（0ms）
#define SIGNAL_INTERVAL_MAX_MS 1000   // 信号间隔最大值（1000ms）
#define SIGNAL_REPEAT_MIN 1           // 重复次数最小值
#define SIGNAL_REPEAT_MAX 10          // 重复次数最大值

// 性能约束
#define FORWARD_DELAY_MAX_MS 50       // A信号接收后B信号发射最大延迟（50ms）
#define INTERVAL_ERROR_MAX_MS 20      // 连续信号间隔最大误差（20ms）
#define MQTT_RESPONSE_MAX_MS 300      // MQTT接口最大响应时间（300ms）
#define INTERRUPT_DISABLE_MAX_MS 10   // 发射时关闭中断最大时长（10ms）

// 网络配置
#define WIFI_RECONNECT_MAX_ATTEMPTS 10  // WiFi最大重连次数
#define WIFI_RECONNECT_DELAY_MS 5000    // WiFi重连延迟（5秒）
#define AP_SSID "ORVIBO-CT30W"          // AP模式SSID
#define AP_PASSWORD "12345678"          // AP模式密码

// MQTT 主题定义
#define MQTT_TOPIC_COMMAND "homeassistant/orvibo-ct30w/command"
#define MQTT_TOPIC_STATE "homeassistant/orvibo-ct30w/state"
#define MQTT_TOPIC_LOG "homeassistant/orvibo-ct30w/log"

// MQTT 配置（优化：减少内存占用）
#define MQTT_BUFFER_SIZE 1024  // MQTT 消息缓冲区大小（从1536减少到1024，节省512字节）

// Home Assistant Discovery 配置
#define MQTT_DISCOVERY_PREFIX "homeassistant"
#define MQTT_DEVICE_ID "orvibo_ct30w"
#define MQTT_DEVICE_NAME "ORVIBO CT30W 红外转发器"

// 线程优先级（FreeRTOS）
#define TASK_PRIORITY_IR_RX 5        // 红外接收（最高）
#define TASK_PRIORITY_IR_TX 4       // 红外发射（中）
#define TASK_PRIORITY_MQTT 2       // MQTT（低）
#define TASK_PRIORITY_NETWORK 1    // 网络配置（最低）

// 存储文件路径
#define CONFIG_FILE_NETWORK "/network_config.json"
#define CONFIG_FILE_MAPS "/ir_maps.json"
#define LOG_FILE "/ir_logs.json"

// 调试宏定义（注释掉以禁用调试输出）
// #define DEBUG_ENABLED

#ifdef DEBUG_ENABLED
    #define DEBUG_PRINT(x) Serial.print(x)
    #define DEBUG_PRINTLN(x) Serial.println(x)
    #define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
    #define DEBUG_PRINT(x)
    #define DEBUG_PRINTLN(x)
    #define DEBUG_PRINTF(...)
#endif

#endif // CONSTANTS_H

