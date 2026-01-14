#ifndef IR_LEARNER_H
#define IR_LEARNER_H

#include <Arduino.h>
#include "config/pins.h"
#include "config/constants.h"
#include "hardware/ir_receiver.h"
#include "ir_parser.h"
#include "utils/logger.h"

// 学习模式类型
enum LearnMode {
    LEARN_NONE,
    LEARN_DEVICE     // 学习设备（API调用）
};

// 学习的指令结构
struct LearnedCommand {
    char name[MAX_COMMAND_NAME_LEN];  // 指令名称
    SignalSequence sequence;          // 信号序列（支持多个信号+间隔，单个信号时signalCount=1）
    bool valid;                       // 是否有效
};

// 学习的设备结构
struct LearnedDevice {
    char deviceName[MAX_DEVICE_NAME_LEN];  // 设备名称
    LearnedCommand commands[MAX_LEARN_COMMANDS];  // 指令列表
    int commandCount;                   // 指令数量
    bool valid;                         // 是否有效
};

// 学习器
class IRLearner {
public:
    IRLearner(IRReceiver& receiver, IRParser& parser, Logger& logger);
    void init();
    void update();
    LearnMode getMode() const;
    
    // API调用的学习模式
    bool startDeviceLearn(const String& deviceName, const String& commandNames, int timeoutSeconds);
    bool stopDeviceLearn();
    bool isLearningDevice() const;
    String getLearnProgress() const;  // 返回学习进度，如"2/5"
    bool getLearnedDevice(LearnedDevice& device);  // 获取学习结果
    
    // 输出格式转换方法
    String convertToStringFormat();      // 转换为字符串序列格式（默认格式）
    String convertToJSONFormat();        // 转换为JSON格式（commands字段的值是信号序列JSON对象）
    
    // Base64解码（用于发送学习到的信号）
    static bool decodeBase64ToTiming(const String& base64, IRTiming* timing, int& count, int maxCount);
    
    // 统一信号格式解析函数（用于HTTP API和MQTT接口）
    enum SignalFormat {
        FORMAT_BASE64,      // Base64字符串
        FORMAT_ARRAY,       // 数组字符串
        FORMAT_PROTOCOL,    // 协议格式字符串 protocol,data,repeatCount
        FORMAT_SIGNALID     // UnifiedSignal对象格式（JSON）
    };
    
    static SignalFormat detectSignalFormat(const String& payload);
    static bool parseArrayString(const String& arrayStr, IRTiming* timing, int& count, int maxCount);
    static bool parseProtocolString(const String& protocolStr, UnifiedSignal& signal, uint8_t& repeatCount);
    static bool parseSignalPayload(const String& payload, UnifiedSignal& signal, IRTiming* timing, int& timingCount, int maxTimingCount, uint8_t& repeatCount, SignalFormat& format);
    
    // 字符串序列格式解析（支持序列）
    static bool parseStringSequence(const String& payload, SignalSequence& sequence, IRTiming* timingBuffer, int maxTimingCount);
    
    // 学习状态查询方法
    String getCurrentCommandName() const;  // 获取当前正在学习的指令名称
    int getLearnedCommandCount() const;   // 获取已学习的指令数量
    int getTotalCommandCount() const;      // 获取总指令数量
    String getDeviceName() const;          // 获取当前学习的设备名称
    void getLearnedCommandNames(String* names, int maxCount) const;  // 获取已学习的指令名称列表
    void getLearnedCommandNames(const char** names, int maxCount) const;  // 获取已学习的指令名称列表（返回const char*指针，节省内存）

private:
    IRReceiver& receiver;
    IRParser& parser;
    Logger& logger;
    
    LearnMode mode;
    
    // 设备学习相关（动态分配，节省启动内存）
    LearnedDevice* learnedDevice;      // 仅在开始学习时分配，完成后保留直到下次学习或手动释放
    int currentCommandIndex;           // 当前学习的指令索引
    char commandNamesList[MAX_LEARN_COMMANDS][MAX_COMMAND_NAME_LEN];  // 指令名称列表（使用char数组节省内存）
    int commandNamesCount;             // 指令名称数量
    unsigned long lastSignalTime;      // 最后接收信号时间
    unsigned long learnTimeoutMs;      // 学习超时时间（毫秒）
    
    // 重复信号检测相关
    UnifiedSignal lastLearnedSignal;   // 上一个学习的信号（用于重复检测）
    bool hasLastSignal;                 // 是否有上一个信号用于比较
    unsigned long lastLearnedSignalTime; // 上一个信号的时间戳（用于计算间隔）
    
    void processDeviceLearnSignal(const IRTiming* timing, int count);
    void checkLearnTimeout();
    String encodeTimingToBase64(const IRTiming* timing, int count);
    void parseCommandNames(const String& commandNamesStr);
    void freeLearnedDevice();  // 释放学习设备内存
};

#endif // IR_LEARNER_H

