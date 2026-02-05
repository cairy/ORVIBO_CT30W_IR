#ifndef CONSTANTS_H
#define CONSTANTS_H

// ============================================================================
// 红外接收缓冲区
// ============================================================================
#define IR_RX_BUFFER_SIZE 2048       // 红外接收缓冲区大小（2KB，用于存储原始红外时序数据）

// ============================================================================
// 映射规则配置（A信号→B信号转发规则）
// ============================================================================
#define MAX_MAP_RULES 20             // 最大映射规则数（优化后每条规则约180字节）
#define MAX_B_SIGNALS_PER_RULE 6     // 每条规则最多触发的连续B信号数

// ============================================================================
// 学习功能配置（红外信号学习与存储）
// ============================================================================
#define MAX_LEARN_COMMANDS 8         // 最大可学习指令数（优化后每个指令约1.8KB）
#define MAX_LEARN_TIMING_COUNT 200   // 每个指令最大时序点数（高/低电平切换次数）
#define MAX_COMMAND_NAME_LEN 32      // 指令名称最大长度
#define MAX_DEVICE_NAME_LEN 64       // 设备名称最大长度
#define MAX_RECENT_SIGNALS 5         // 最近接收信号记录数（用于学习时显示候选信号）
#define DEFAULT_LEARN_TIMEOUT_SEC 30 // 学习模式超时时间（秒）

// ============================================================================
// 信号处理参数（红外信号识别与重复码处理）
// ============================================================================
#define SIGNAL_TIMEOUT_MS 500        // 连续信号识别超时（超过此时间视为新信号）
#define SIGNAL_INTERVAL_MAX_MS 1000  // 信号间隔最大值（用于判断是否为连续信号序列）
#define SIGNAL_INTERVAL_RESET_THRESHOLD_MS 5000  // 信号间隔重置阈值（超过5秒清除历史状态）
#define SIGNAL_REPEAT_MAX 10         // 单信号最大重复发送次数
#define REPEAT_CODE_INTERVAL_MIN_MS 100  // NEC重复码间隔下限（用于识别长按）
#define REPEAT_CODE_INTERVAL_MAX_MS 120  // NEC重复码间隔上限（NEC协议约108ms）
#define NEC_REPEAT_CODE 0xFFFFFFFF   // NEC重复码标识值（32位全1）

// ============================================================================
// 性能约束（确保实时性的关键参数）
// ============================================================================
#define FORWARD_DELAY_BEFORE_SEND_MS 10  // 转发前延时（AB信号隔离，防止干扰）
#define INTERRUPT_DISABLE_MAX_MS 10  // 发射时关闭中断最大时长（避免影响接收）

// ============================================================================
// 日志配置
// ============================================================================
#define MAX_LOG_ENTRIES 10           // 最大日志条目数（环形缓冲，约850字节）
#define MAX_LOG_TYPE_LEN 16          // 日志类型字段最大长度
#define MAX_LOG_CONTENT_LEN 64       // 日志内容字段最大长度

// ============================================================================
// WiFi 网络配置
// ============================================================================
#define WIFI_RECONNECT_MAX_ATTEMPTS 10  // WiFi断线最大重连次数
#define WIFI_RECONNECT_DELAY_MS 5000    // WiFi重连间隔（5秒）
#define AP_SSID "ORVIBO-CT30W"          // 配网模式AP热点名称
#define AP_PASSWORD "12345678"          // 配网模式AP密码

// ============================================================================
// MQTT 配置
// ============================================================================
// 主题定义
#define MQTT_TOPIC_COMMAND "homeassistant/orvibo-ct30w/command"  // 接收命令主题
#define MQTT_TOPIC_STATE "homeassistant/orvibo-ct30w/state"      // 状态上报主题
#define MQTT_TOPIC_LOG "homeassistant/orvibo-ct30w/log"          // 日志上报主题

// 缓冲区
#define MQTT_BUFFER_SIZE 2048        // MQTT消息缓冲区大小（支持较大的JSON响应）

// Home Assistant Discovery 配置
#define MQTT_DISCOVERY_PREFIX "homeassistant"    // HA自动发现主题前缀
#define MQTT_DEVICE_ID "orvibo_ct30w"            // 设备唯一标识
#define MQTT_DEVICE_NAME "ORVIBO CT30W 红外转发器"  // 设备显示名称

// ============================================================================
// 存储文件路径（SPIFFS/LittleFS）
// ============================================================================
#define CONFIG_FILE_NETWORK "/network_config.json"  // WiFi/MQTT配置文件
#define CONFIG_FILE_MAPS "/ir_maps.json"            // 映射规则配置文件
#define LOG_FILE "/ir_logs.json"                    // 日志存储文件

// ============================================================================
// 内存管理阈值
// ============================================================================
#define MEMORY_MIN_FREE_HEAP 1024        // 最小空闲堆内存（字节）
#define MEMORY_MIN_FOR_JSON 2048         // JSON操作最小内存（字节）
#define MEMORY_MIN_FOR_MQTT 2048         // MQTT操作最小内存（字节）
#define MEMORY_MIN_FOR_WEBSERVER 4096    // Web服务器最小内存（字节）
#define MEMORY_MIN_FOR_FLASH_WRITE 4096  // Flash写入最小内存（字节）
#define MEMORY_WEBSERVER_STOP_PERCENT 92.0f  // Web服务器停止阈值（内存使用率）

// ============================================================================
// 调试宏（取消下行注释以启用串口调试输出）
// ============================================================================
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
