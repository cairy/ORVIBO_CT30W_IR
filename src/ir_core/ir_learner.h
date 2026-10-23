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

// 学习的设备结构（v1.3.41：指令数组按**本次实际条数**分配）
//
// ⚠️⚠️ 以前 `commands` 是内联数组 `LearnedCommand[MAX_LEARN_COMMANDS]` ——
//   8 槽 × 6 信号 ≈ **1.99 KB**，**不管学几条都占满**。实测（2026-10-04）：
//   学 2 条时序码把空闲堆从 8,224 打到 4,568（掉 3.2 KB，永远不回收）。
//   而 4.5 KB 已经低到让 SDK 在**信标路径**上分配失败 ⇒ `Exception (29)` 重启
//   ⇒ **只存在 RAM 的学习结果被清空**（用户看到的就是"学完一场空"）；
//   同一状态下 `POST /api/devices` 的门槛 `freeHeap ≥ body×2+2048` 也必然过不去。
//   改成指针 + 按需分配后，学 2 条只占 ~500 B（省约 1.4 KB）。
// ⛔ 因此 `commands` 的所有权归 IRLearner：**别再把 LearnedDevice 按值拷出去**
//   （浅拷贝会让两个对象持同一块内存，先析构的那个一释放、另一个就悬空）。
struct LearnedDevice {
    char deviceName[MAX_DEVICE_NAME_LEN];  // 设备名称
    LearnedCommand* commands;              // 指令列表（长度 = commandCapacity，可能为 nullptr）
    int commandCapacity;                   // commands 的实际长度（= 本次要学的指令条数）
    int commandCount;                      // 已学习到的指令数量
    bool valid;                            // 是否有效
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
    // ⛔ v1.3.41 删除了 `bool getLearnedDevice(LearnedDevice&)`：它按值拷贝 LearnedDevice，
    //   而该结构现在持有 `LearnedCommand* commands`（独占）⇒ 浅拷贝必悬空。
    //   它在本仓库里从未被调用过（grep 全库只有声明与定义）。要取结果用
    //   `getLearnedCommandCount()` / `getLearnedCommandNames()` / `convertToStringFormat()`。
    
    // 输出格式转换方法
    String convertToStringFormat();      // 转换为字符串序列格式（默认格式）
    String convertToJSONFormat();        // 转换为JSON格式（commands字段的值是信号序列JSON对象）

    // ── 学习结果落盘（v1.3.46）────────────────────────────────────────────
    // ⭐⭐ 学完就把结果写进 LittleFS 并**立刻释放** RAM 里的 LearnedDevice。
    //   理由（2026-10-04 实测，见 DETAIL.md §3）：学 2 条时序码会把空闲堆从 ~8 KB
    //   打到 ~4.5 KB，而 4.5 KB 已经低到 SDK 在**信标路径**上分配失败 ⇒ 学完 3~4 分钟
    //   设备自行 `Exception (29)` 重启 ⇒ **只存在 RAM 的学习结果被清空**（4 次里崩 2 次）。
    //   落盘后：① 堆回到 ~7.8 KB（不再崩）；② 重启/断电/OTA 都不再丢结果。
    // ⛔ 落盘成功后 `learnedDevice` 即为 nullptr —— 别假设它一直在。
    bool isResultPersisted() const;      // 结果已在 flash 里（RAM 那份已释放）
    // 读回落盘的结果文本。jsonFormat=false 取 string 格式（面板用），true 取 json 格式。
    // 对外格式与 convertToStringFormat()/convertToJSONFormat() 的输出**逐字相同**。
    // ⛔ 只有 isResultPersisted() 为真时才有内容可读。
    String loadPersistedResult(bool jsonFormat = false) const;

    // 清除学习结果（v1.3.50）：删掉落盘的三个文件 + 清 RAM 计数/缓冲。
    // 面板上的「清除学习结果」按钮走它。返回"是否真的删掉了什么"。
    // ⚠️ 学习进行中拒绝清除（否则刚学的会被清掉、界面显示与实际不一致）⇒ 返回 false。
    bool clearPersistedResult();

    // Base64解码（用于发送学习到的信号）- 使用 SignalUtils::base64ToTiming()
    // 保留此声明以保持API兼容性，内部调用 SignalUtils::base64ToTiming()
    static bool decodeBase64ToTiming(const String& base64, IRTiming* timing, int& count, int maxCount);
    
    // 统一信号格式解析函数（用于HTTP API和MQTT接口）
    enum SignalFormat {
        FORMAT_BASE64,      // Base64字符串
        FORMAT_ARRAY,       // 数组字符串
        FORMAT_PROTOCOL,    // 协议格式字符串 protocol,data,repeatCount
        FORMAT_SIGNALID     // UnifiedSignal对象格式（JSON）
    };
    
    static SignalFormat detectSignalFormat(const String& payload);
    // 指令名分隔符归一化：把中文「，」「、」「；」与半角「;」都收成半角逗号。
    // 老实现只认半角 ','，中文输入法下打出来的「开机，关机」会被当成**一个**指令名。
    static String normalizeCommandNames(const String& in);
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
    LearnedDevice* learnedDevice;      // 仅在开始学习时分配，落盘成功后立刻释放（见 persistLearnedResult）
    int currentCommandIndex;           // 当前学习的指令索引
    char commandNamesList[MAX_LEARN_COMMANDS][MAX_COMMAND_NAME_LEN];  // 指令名称列表（使用char数组节省内存）
    int commandNamesCount;             // 指令名称数量
    unsigned long lastSignalTime;      // 最后接收信号时间
    unsigned long learnTimeoutMs;      // 学习超时时间（毫秒）
    
    // 重复信号检测相关
    UnifiedSignal lastLearnedSignal;   // 上一个学习的信号（用于重复检测）
    bool hasLastSignal;                 // 是否有上一个信号用于比较
    unsigned long lastLearnedSignalTime; // 上一个信号的时间戳（用于计算间隔）
    
    int persistedCommandCount;          // 已落盘的指令条数（learnedDevice 释放后仍能回答"学过几条"）
    
    void processDeviceLearnSignal(const IRTiming* timing, int count);
    void checkLearnTimeout();
    void parseCommandNames(const String& commandNamesStr);
    void freeLearnedDevice();  // 释放学习设备内存
    // v1.3.47：启动时从 CONFIG_FILE_LEARNED_COUNT + CONFIG_FILE_LEARNED 恢复"学过几条"，
    // 否则重启后 persistedCommandCount 归零 ⇒ /learn/result 报 404（文件明明还在）。
    void restorePersistedCount();
    // 学完调用：把结果写进 CONFIG_FILE_LEARNED；成功则 freeLearnedDevice() 并把条数记进 persistedCommandCount。
    // 失败（写完不进去）时**什么都不动** —— 保持"结果还在 RAM"的老行为，不做半吊子释放。
    bool persistLearnedResult();
};

#endif // IR_LEARNER_H

