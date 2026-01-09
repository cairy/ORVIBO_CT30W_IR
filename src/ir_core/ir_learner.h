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
    SignalID signalId;                // 信号ID
    IRTiming timing[MAX_LEARN_TIMING_COUNT];  // 时序数据（受内存限制）
    int timingCount;                  // 时序数量
    bool valid;                       // 是否有效
};

// 学习的设备结构（兼容SmartIR格式）
struct LearnedDevice {
    char deviceName[MAX_DEVICE_NAME_LEN];  // 设备名称
    LearnedCommand commands[MAX_LEARN_COMMANDS];  // 指令列表
    int commandCount;                   // 指令数量
    unsigned long learnTime;            // 学习时间
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
    String convertToSmartIRFormat();  // 转换为SmartIR格式JSON

private:
    IRReceiver& receiver;
    IRParser& parser;
    Logger& logger;
    
    LearnMode mode;
    
    // 设备学习相关
    LearnedDevice learnedDevice;
    int currentCommandIndex;           // 当前学习的指令索引
    char commandNamesList[MAX_LEARN_COMMANDS][MAX_COMMAND_NAME_LEN];  // 指令名称列表（使用char数组节省内存）
    int commandNamesCount;             // 指令名称数量
    unsigned long lastSignalTime;      // 最后接收信号时间
    unsigned long learnTimeoutMs;      // 学习超时时间（毫秒）
    
    void processDeviceLearnSignal();
    void checkLearnTimeout();
    String encodeTimingToBase64(const IRTiming* timing, int count);
    void parseCommandNames(const String& commandNamesStr);
};

#endif // IR_LEARNER_H

