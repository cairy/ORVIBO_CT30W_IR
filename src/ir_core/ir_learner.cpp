#include "ir_learner.h"
#include "utils/hex_utils.h"
#include "utils/timing_scratch.h"
#include "utils/storage.h"      // v1.3.46：学习结果落盘（CONFIG_FILE_LEARNED）
#include "utils/memory_monitor.h"
#include "unified_signal.h"
#include <ArduinoJson.h>
#include <string.h>
#include <utility>      // std::move（JSON 扩容时转移内存池的所有权，而不是深拷贝）


IRLearner::IRLearner(IRReceiver& receiver, IRParser& parser, Logger& logger)
    : receiver(receiver), parser(parser), logger(logger),
      mode(LEARN_NONE),
      learnedDevice(nullptr),
      currentCommandIndex(0), commandNamesCount(0), lastSignalTime(0), learnTimeoutMs(0),
      hasLastSignal(false), lastLearnedSignalTime(0),
      persistedCommandCount(0) {
    lastLearnedSignal.clear();
}

void IRLearner::init() {
    mode = LEARN_NONE;
    freeLearnedDevice();  // 确保没有遗留的内存
    // ⭐ v1.3.47：上一次会话落盘的结果要能**跨重启**读到。
    //   条数单独存一个小文件（几字节），因为 persistedCommandCount 是 RAM 变量、重启即归零
    //   —— 只留 JSON 的话，重启后 getLearnedCommandCount() 回 0，/learn/result 报
    //   404「No learned device found」（2026-10-04 实测踩过这一条）。
    restorePersistedCount();
}

void IRLearner::restorePersistedCount() {
    const int count = Storage::readFile(CONFIG_FILE_LEARNED_COUNT).toInt();
    if (count <= 0 || !Storage::fileExists(CONFIG_FILE_LEARNED)) {
        // 计数与文件必须成对：只有计数没有内容（或反之）⇒ 一律当"没有结果"，
        // 免得读出一份空内容却对外声称学过几条。
        persistedCommandCount = 0;
        return;
    }
    persistedCommandCount = count;
    Serial.printf_P(PSTR("[IRLearner] 恢复上次落盘的学习结果：%d 条（重启后仍可读取）\n"), count);
}

void IRLearner::update() {
    // 检查设备学习超时
    if (mode == LEARN_DEVICE) {
        checkLearnTimeout();
    }
    
    if (mode == LEARN_DEVICE) {
        // 设备学习模式：复用全局共享的时序临时缓冲（原先在函数内单独常驻 1,600 B）
        // 见 src/utils/timing_scratch.h —— 学到的信号会在 processDeviceLearnSignal 里
        // 转成 Base64 存进 LearnedCommand，不会跨调用持有这块缓冲。
        IRTiming* timing = timingScratch();
        int count = 0;
        
        if (receiver.getSignal(timing, count, TIMING_SCRATCH_CAPACITY)) {
            lastSignalTime = millis();
            processDeviceLearnSignal(timing, count);
        }
    }
}

LearnMode IRLearner::getMode() const {
    return mode;
}

bool IRLearner::startDeviceLearn(const String& deviceName, const String& commandNames, int timeoutSeconds) {
    if (mode != LEARN_NONE) {
        return false;  // 已有学习任务进行中
    }
    
    // 粗略预检：LearnedDevice 本体在 v1.3.41 之后已很小（~80 B），这里只挡住
    // "连壳都分配不出来"的情况。**真正按指令条数算的检查在 parseCommandNames() 之后**
    // —— 那时才知道这次要学几条，也才知道要分配多大的 commands 数组。
    size_t freeHeap = ESP.getFreeHeap();
    size_t requiredSize = sizeof(LearnedDevice);
    if (freeHeap < requiredSize + 512) {
        Serial.printf_P(PSTR("[IRLearner] 内存不足，无法开始学习 (需要: %u bytes, 可用: %u bytes)\n"),
                     (unsigned)requiredSize, (unsigned)freeHeap);
        return false;
    }
    
    // 释放之前的学习数据（如果存在）
    freeLearnedDevice();
    // ⭐ v1.3.46：上一轮落盘的结果也要清掉 —— 否则"开始新的学习"之后 /learn/result 仍会
    //   回上一次的结果，而语义一直是"开始学习即重置"（老实现靠 commandCount==0 表达）。
    if (persistedCommandCount > 0 || Storage::fileExists(CONFIG_FILE_LEARNED)) {
        Storage::deleteFile(CONFIG_FILE_LEARNED);
    }
    Storage::deleteFile(CONFIG_FILE_LEARNED_JSON);
    Storage::deleteFile(CONFIG_FILE_LEARNED_COUNT);   // ⭐ v1.3.47：条数标记一起清（语义 = 开始学习即重置）
    persistedCommandCount = 0;
    
    // 分配新的学习设备内存
    learnedDevice = new (std::nothrow) LearnedDevice();
    if (learnedDevice == nullptr) {
        Serial.printf_P(PSTR("[IRLearner] 内存分配失败，无法开始学习 (需要: %u bytes)\n"), (unsigned)requiredSize);
        return false;
    }
    
    // 初始化学习设备
    learnedDevice->valid = false;
    learnedDevice->commandCount = 0;
    learnedDevice->commands = nullptr;      // ⭐ 必须显式置空：下面几条 early return 之后
    learnedDevice->commandCapacity = 0;     //    freeLearnedDevice() 要靠它判断要不要 delete[]
    learnedDevice->deviceName[0] = '\0';
    
    // ⚠️ 切换中断【必须】放在所有校验之后（见下方 mode = LEARN_DEVICE 之前那两行）。
    // 以前这两行在函数最前面，于是上面任何一条 early return（指令数超限、无有效指令名）
    // 都会留下"库接收已 detach、自定义接收又没人挂"的状态 ⇒ 任何红外都收不到、
    // 转发全停，且无日志、无状态位，只能靠重启恢复。
    
    // 设置设备名称
    const char* deviceNameStr = deviceName.c_str();
    if (deviceNameStr != nullptr) {
        strncpy(learnedDevice->deviceName, deviceNameStr, MAX_DEVICE_NAME_LEN - 1);
        learnedDevice->deviceName[MAX_DEVICE_NAME_LEN - 1] = '\0';
    } else {
        learnedDevice->deviceName[0] = '\0';
    }
    
    
    // 分隔符归一化：中文「，」「、」「；」与半角「;」都收成半角逗号，再往下一切照旧。
    // 必须在这里做（而不是在 parseCommandNames 里）—— 下面这段"先数一遍够不够上限"
    // 的循环也在按逗号切，两处口径必须一致，否则上限判断会少算。
    const String names = normalizeCommandNames(commandNames);

    // 先统计命令数量，检查是否超过限制
    int commandCount = 0;
    if (names.length() > 0) {
        int startPos = 0;
        int commaPos = names.indexOf(',');
        while (commaPos >= 0) {
            String cmdName = names.substring(startPos, commaPos);
            cmdName.trim();
            if (cmdName.length() > 0) {
                commandCount++;
            }
            startPos = commaPos + 1;
            commaPos = names.indexOf(',', startPos);
        }
        // 处理最后一个命令
        if (startPos < names.length()) {
            String cmdName = names.substring(startPos);
            cmdName.trim();
            if (cmdName.length() > 0) {
                commandCount++;
            }
        }
    }
    
    // 检查指令数量是否超过限制
    if (commandCount > MAX_LEARN_COMMANDS) {
        Serial.println("错误：指令数量超过限制，最多支持 " + String(MAX_LEARN_COMMANDS) + " 个指令，当前有 " + String(commandCount) + " 个");
        return false;
    }
    
    // 解析指令名称列表
    parseCommandNames(names);
    
    if (commandNamesCount == 0) {
        Serial.println(F("错误：没有有效的指令名称"));
        return false;  // 没有有效的指令名称
    }

    // ⭐⭐ 按**本次实际条数**分配指令数组（v1.3.41）。
    //   以前 `commands` 是内联的 `LearnedCommand[MAX_LEARN_COMMANDS]`（8 槽 ≈ 1.99 KB），
    //   学 2 条也照样占满 —— 实测学完只剩 4.5 KB 空闲堆，SDK 会自己崩（见 ir_learner.h 的说明）。
    //   这里是**第一个知道"要学几条"**的位置（parseCommandNames 刚填完），所以检查与分配都放这儿。
    const size_t cmdBytes = sizeof(LearnedCommand) * (size_t)commandNamesCount;
    if (ESP.getFreeHeap() < cmdBytes + 1024) {
        Serial.printf_P(PSTR("[IRLearner] 内存不足，无法按 %d 条指令分配学习记录区 (需要: %u bytes, 可用: %u bytes)\n"),
                      commandNamesCount, (unsigned)cmdBytes, (unsigned)ESP.getFreeHeap());
        return false;
    }
    learnedDevice->commands = new (std::nothrow) LearnedCommand[commandNamesCount];
    if (learnedDevice->commands == nullptr) {
        Serial.printf_P(PSTR("[IRLearner] 学习记录区分配失败 (需要: %u bytes)\n"), (unsigned)cmdBytes);
        return false;
    }
    learnedDevice->commandCapacity = commandNamesCount;
    for (int i = 0; i < commandNamesCount; i++) {
        learnedDevice->commands[i].valid = false;
        learnedDevice->commands[i].name[0] = '\0';
    }
    
    // 到这里所有校验都已通过，此刻才把 GPIO5 的中断从 IRrecv 切给自定义接收器。
    // 学习模式要的是原始时序，必须自己采样边沿。
    parser.disableLibraryReceiver();
    receiver.init();
    receiver.attach();

    // 初始化学习状态
    mode = LEARN_DEVICE;
    currentCommandIndex = 0;
    learnTimeoutMs = timeoutSeconds > 0 ? timeoutSeconds * 1000UL : DEFAULT_LEARN_TIMEOUT_SEC * 1000UL;
    lastSignalTime = millis();
    
    // 重置重复信号检测
    hasLastSignal = false;
    lastLearnedSignal.clear();
    lastLearnedSignalTime = 0;
    
    // 清空接收缓冲区
    receiver.clearBuffer();
    
    logger.logLearn("开始学习设备: " + deviceName + ", 指令数: " + String(commandNamesCount));
    Serial.println(F("=== 开始学习设备 ==="));
    Serial.println("设备名称: " + deviceName);
    Serial.println("指令列表: " + names);
    if (commandNamesCount > 0 && commandNamesList[0][0] != '\0') {
        Serial.println("请发送第1个指令: " + String(commandNamesList[0]));
    } else {
        Serial.println(F("请发送第1个指令"));
    }
    
    return true;
}

bool IRLearner::stopDeviceLearn() {
    if (mode != LEARN_DEVICE || learnedDevice == nullptr) {
        return false;
    }
    
    // 标记学习完成（即使没有完成所有指令）
    learnedDevice->valid = (learnedDevice->commandCount > 0);
    
    mode = LEARN_NONE;
    
    // 交还 GPIO5：先摘掉自定义 ISR，再让 IRrecv 重新挂上。
    // 只调 enableLibraryReceiver() 也行（attach 会覆盖），但若 irrecv 为空就没人接管了。
    receiver.detach();
    parser.enableLibraryReceiver();
    
    String result = learnedDevice->valid ? 
        ("学习完成，共学习 " + String(learnedDevice->commandCount) + " 个指令") :
        "学习已取消";
    
    logger.logLearn(result);
    Serial.println("=== " + result + " ===");
    
    // 清空重复信号检测
    hasLastSignal = false;
    lastLearnedSignal.clear();
    lastLearnedSignalTime = 0;
    
    // ⭐⭐ v1.3.46：学完立刻把结果落盘并**释放内存**。
    //   这里以前写的是"不立即释放内存，保留数据用于转换函数" —— 那正是崩溃的根因：
    //   学 2 条时序码把空闲堆打到 ~4.5 KB，设备随后在**空闲**状态下 3~4 分钟内
    //   自行 `Exception (29)` 重启（SDK 在信标路径上分配失败，栈里没有我们的帧），
    //   而结果只存在 RAM ⇒ 用户"学完一场空"（实测 4 次里崩 2 次，见 DETAIL.md §3）。
    //   落盘成功后堆回到 ~7.8 KB；读结果改从文件走；重启/断电/OTA 也不再丢。
    //   ⛔ 落盘失败时**保持原样**（结果仍留在 RAM），不做半吊子释放。
    persistLearnedResult();

    return true;
}

bool IRLearner::isLearningDevice() const {
    return mode == LEARN_DEVICE;
}

String IRLearner::getLearnProgress() const {
    if (mode != LEARN_DEVICE) {
        return "";
    }
    
    // 优化：使用reserve预分配内存，减少内存碎片
    String result;
    result.reserve(16);  // 预分配足够空间（如"5/10"）
    result = String(currentCommandIndex + 1);
    result += "/";
    result += String(commandNamesCount);
    return result;
}

// ⛔ v1.3.41：`getLearnedDevice(LearnedDevice&)` 已删除。
//   它做的是 `device = *learnedDevice;` —— 而 LearnedDevice 现在持有独占的
//   `LearnedCommand* commands`，浅拷贝会让两个对象指向同一块，先析构的那个一释放、
//   另一个就悬空。该函数在本仓库里从未被调用（全库只有声明+定义），删掉最省事；
//   取结果请走 `getLearnedCommandCount()` / `getLearnedCommandNames()` / `convertToStringFormat()`。

String IRLearner::getCurrentCommandName() const {
    if (mode != LEARN_DEVICE) {
        return "";
    }
    
    if (currentCommandIndex >= 0 && currentCommandIndex < commandNamesCount && 
        currentCommandIndex < MAX_LEARN_COMMANDS) {
        return String(commandNamesList[currentCommandIndex]);
    }
    
    return "";
}

int IRLearner::getLearnedCommandCount() const {
    // 学习完成后，mode 变为 LEARN_NONE，但 learnedDevice 仍然有效
    // 所以需要检查 learnedDevice->valid 而不是 mode
    // ⭐ v1.3.46：落盘后 learnedDevice 已被释放，条数改由 persistedCommandCount 回答 ——
    //   否则 handleQueryLearnResult() 会把"有结果"误判成 NO_DATA（404"你压根没学过"）。
    if (learnedDevice == nullptr || !learnedDevice->valid) {
        return persistedCommandCount;
    }
    
    return learnedDevice->commandCount;
}

int IRLearner::getTotalCommandCount() const {
    if (mode != LEARN_DEVICE) {
        return 0;
    }
    
    return commandNamesCount;
}

String IRLearner::getDeviceName() const {
    if (mode != LEARN_DEVICE || learnedDevice == nullptr) {
        return "";
    }
    
    return String(learnedDevice->deviceName);
}

void IRLearner::getLearnedCommandNames(String* names, int maxCount) const {
    if (names == nullptr || maxCount <= 0 || learnedDevice == nullptr) {
        return;
    }
    
    int count = 0;
    for (int i = 0; i < learnedDevice->commandCount && i < maxCount && i < learnedDevice->commandCapacity; i++) {
        if (learnedDevice->commands[i].valid) {
            names[count++] = String(learnedDevice->commands[i].name);
        }
    }
    
    // 填充剩余空间为空字符串
    for (int i = count; i < maxCount; i++) {
        names[i] = "";
    }
}

void IRLearner::getLearnedCommandNames(const char** names, int maxCount) const {
    if (names == nullptr || maxCount <= 0 || learnedDevice == nullptr) {
        return;
    }
    
    int count = 0;
    for (int i = 0; i < learnedDevice->commandCount && i < maxCount && i < learnedDevice->commandCapacity; i++) {
        if (learnedDevice->commands[i].valid) {
            names[count++] = learnedDevice->commands[i].name;
        }
    }
    
    // 填充剩余空间为nullptr
    for (int i = count; i < maxCount; i++) {
        names[i] = nullptr;
    }
}

void IRLearner::processDeviceLearnSignal(const IRTiming* timing, int count) {
    if (currentCommandIndex >= commandNamesCount || currentCommandIndex >= MAX_LEARN_COMMANDS) {
        return;  // 所有指令已学习完成
    }
    
    if (timing == nullptr || count <= 0) {
        Serial.println(F("[学习] 错误：时序数据无效"));
        return;
    }
    
    // 验证信号长度
    const int MIN_VALID_TIMING_COUNT = 10;
    if (count < MIN_VALID_TIMING_COUNT) {
        Serial.println(F("错误：信号太短，无法学习"));
        return;
    }
    
    // 检查信号是否超过最大长度限制
    int actualTimingCount = count;
    if (count > MAX_LEARN_TIMING_COUNT) {
        actualTimingCount = MAX_LEARN_TIMING_COUNT;
        Serial.printf_P(PSTR("警告：信号长度 %d 超过最大限制 %d，将被截断\n"), count, MAX_LEARN_TIMING_COUNT);
    }
    
    // 保存学习的指令
    //   v1.3.41：边界改成 commandCapacity（= 本次要学的条数），并显式挡住
    //   "记录区没分配出来"的情况 —— 上面 commandCount=0 时本函数进不来，
    //   但早退路径会留下 commands==nullptr，多一道更稳。
    if (learnedDevice == nullptr || learnedDevice->commands == nullptr ||
        currentCommandIndex >= learnedDevice->commandCapacity) {
        Serial.println(F("错误：指令索引超出范围（或学习记录区未分配）"));
        return;
    }
    
    LearnedCommand& cmd = learnedDevice->commands[currentCommandIndex];
    if (currentCommandIndex < commandNamesCount) {
        strncpy(cmd.name, commandNamesList[currentCommandIndex], MAX_COMMAND_NAME_LEN - 1);
        cmd.name[MAX_COMMAND_NAME_LEN - 1] = '\0';
    } else {
        cmd.name[0] = '\0';
    }
    
    // 尝试解析协议（可选，不影响学习结果）
    UnifiedSignal signal;
    bool parseSuccess = parser.parseSignal(timing, actualTimingCount, signal);
    
    if (!parseSuccess) {
        // 解析失败，使用时序码
        signal.clear();
        signal.useTiming = true;  // 时序码信号
        signal.timingBase64 = SignalUtils::timingToBase64(timing, actualTimingCount);
        signal.timingCount = actualTimingCount;
    } else {
        // 解析成功，保存UnifiedSignal
        // 如果解析成功但使用时序码，保存时序码Base64
        if (signal.useTiming) {
            signal.timingBase64 = SignalUtils::timingToBase64(timing, actualTimingCount);
        }
    }
    
    // 序列检测逻辑：判断是否加入当前序列
    unsigned long currentTime = millis();
    unsigned long interval = 0;
    if (hasLastSignal && lastLearnedSignalTime > 0) {
        // 无符号减法本身就正确处理回绕（millis() 约 49.7 天回绕一次）。
        // 手写的 "currentTime < last ⇒ 0xFFFFFFFF - last + currentTime" 分支不仅多余，
        // 算出来还比正确值**小 1 毫秒**。
        interval = currentTime - lastLearnedSignalTime;
    }
    
    // 初始化序列（如果还没有）
    if (cmd.sequence.signalCount == 0) {
        cmd.sequence.clear();
        cmd.sequence.timestamp = currentTime;
    }
    
    // 在判断是否加入序列之前，先检查是否为重复码
    if (interval > 0 && interval < SIGNAL_INTERVAL_MAX_MS && cmd.sequence.signalCount > 0) {
        // 检查是否为重复码：信号相同且间隔在重复码范围内
        const UnifiedSignal& lastSignal = cmd.sequence.signals[cmd.sequence.signalCount - 1];
        if (SignalUtils::signalsEqual(signal, lastSignal) && 
            interval >= REPEAT_CODE_INTERVAL_MIN_MS && 
            interval <= REPEAT_CODE_INTERVAL_MAX_MS) {
            // 是重复码，增加上一个信号的repeatCount
            if (cmd.sequence.signals[cmd.sequence.signalCount - 1].repeatCount < SIGNAL_REPEAT_MAX) {
                cmd.sequence.signals[cmd.sequence.signalCount - 1].repeatCount++;
            }
            // 更新时间戳，但不添加新信号
            lastLearnedSignal = signal;
            hasLastSignal = true;
            lastLearnedSignalTime = currentTime;
            return;  // 直接返回，不继续处理
        }
    }
    
    // 判断是否加入序列
    if (interval > 0 && interval < SIGNAL_INTERVAL_MAX_MS && cmd.sequence.signalCount < MAX_B_SIGNALS_PER_RULE) {
        // 加入序列（标准模式：interval 是前一个信号和当前信号之间的间隔）
        cmd.sequence.addSignal(signal, interval, false);
    } else {
        // 开始新序列（单个信号也是序列）
        cmd.sequence.clear();
        cmd.sequence.timestamp = currentTime;
        cmd.sequence.addSignal(signal, 0, false);
    }
    
    // 重复信号检测
    if (hasLastSignal && SignalUtils::signalsEqual(signal, lastLearnedSignal)) {
        String signalDesc = parser.signalToString(signal);
        String logMsg = "检测到重复信号: " + signalDesc + "，间隔: " + String(interval) + "ms";
        logger.logLearn(logMsg);
        Serial.println("[学习] " + logMsg);
    }
    
    Serial.println(F("=== 指令学习完成 ==="));
    Serial.println("指令名称: " + String(cmd.name));
    Serial.println("信号: " + parser.signalToString(signal));
    if (cmd.sequence.signalCount > 1) {
        Serial.printf_P(PSTR("序列长度: %d 个信号\n"), cmd.sequence.signalCount);
    }
    
    // 更新上一个学习的信号（无论是否重复都保存）
    lastLearnedSignal = signal;
    hasLastSignal = true;
    lastLearnedSignalTime = currentTime;
    
    cmd.valid = true;
    learnedDevice->commandCount = currentCommandIndex + 1;
    currentCommandIndex++;
    
    // 检查是否所有指令都已学习
    if (currentCommandIndex >= commandNamesCount) {
        // 所有指令学习完成
        logger.logLearn("设备学习完成: " + String(learnedDevice->deviceName) + 
                       ", 共 " + String(learnedDevice->commandCount) + " 个指令");
        Serial.println(F("=== 所有指令学习完成 ==="));
        // ⭐⭐ v1.3.47（关键修复）：这里原本自己把 valid / mode / GPIO 都做一遍就**直接返回**，
        //   绕过了 stopDeviceLearn() ⇒ 也就绕过了它内部的 persistLearnedResult()
        //   ⇒ **正常学完最后一条时，结果从未落盘、那块学习缓冲从未释放**。
        //   实测后果（2026-10-04，v1.3.46）：学 2 条 170 点后堆停在 ~4.3 KB，
        //   45 秒后 SDK 在信标路径上 `Exception (29)` 重启 ⇒ 结果全丢，
        //   用户看到的就是"学习完成之后还是不显示记录"。
        //   ⛔ 学习结束只能有这一个出口：不管"学满自动结束 / 超时 / 用户点停止"，
        //     都必须经过 stopDeviceLearn()（落盘 + 释放 + 交还 GPIO + 清重复检测都在里面）。
        stopDeviceLearn();
    } else {
        // 继续学习下一个指令
        lastSignalTime = millis();
        if (currentCommandIndex < commandNamesCount && currentCommandIndex < MAX_LEARN_COMMANDS) {
            Serial.println("请发送第" + String(currentCommandIndex + 1) + "个指令: " + 
                          String(commandNamesList[currentCommandIndex]));
        } else {
            Serial.println("请发送第" + String(currentCommandIndex + 1) + "个指令");
        }
    }
}

void IRLearner::checkLearnTimeout() {
    if (mode != LEARN_DEVICE) {
        return;
    }
    
    unsigned long now = millis();
    unsigned long elapsed = now - lastSignalTime;
    
    if (elapsed >= learnTimeoutMs) {
        // 超时，结束学习
        Serial.println(F("学习超时，自动结束学习"));
        stopDeviceLearn();
    }
}

// 指令名分隔符归一化。
// ⚠️ 老实现只认**半角逗号** ','。中文输入法下打出来的是全角「，」（UTF-8 EF BC 8C），
//   于是「开机，关机」被当成**一个**指令名 —— 串口里 `指令列表: 开机，关机` 看着正常，
//   但 `指令数: 1`，用户学完只会拿到一条。中文场景里「、」和「；」同样是"并列"的常见写法，
//   一并收进来；半角「;」也收，免得两种分号各认一半。
//   `String::replace` 是按字节匹配的，UTF-8 多字节 needle 直接可用（源文件本身是 UTF-8）。
String IRLearner::normalizeCommandNames(const String& in) {
    String out = in;
    out.replace("，", ",");   // 全角逗号
    out.replace("、", ",");   // 顿号
    out.replace("；", ",");   // 全角分号
    out.replace(";", ",");    // 半角分号
    out.replace("　", " ");   // 全角空格（trim 只认 ASCII 空白）
    return out;
}

void IRLearner::parseCommandNames(const String& commandNamesStr) {
    commandNamesCount = 0;
    
    // 初始化数组，确保所有元素都是空字符串
    for (int i = 0; i < MAX_LEARN_COMMANDS; i++) {
        commandNamesList[i][0] = '\0';
    }
    
    if (commandNamesStr.length() == 0) {
        return;
    }
    
    int startPos = 0;
    int commaPos = commandNamesStr.indexOf(',');
    
    while (commaPos >= 0 && commandNamesCount < MAX_LEARN_COMMANDS) {
        String cmdName = commandNamesStr.substring(startPos, commaPos);
        cmdName.trim();
        if (cmdName.length() > 0 && cmdName.length() < MAX_COMMAND_NAME_LEN) {
            const char* cmdNameStr = cmdName.c_str();
            if (cmdNameStr != nullptr) {
                strncpy(commandNamesList[commandNamesCount], cmdNameStr, MAX_COMMAND_NAME_LEN - 1);
                commandNamesList[commandNamesCount][MAX_COMMAND_NAME_LEN - 1] = '\0';
                commandNamesCount++;
            }
        }
        startPos = commaPos + 1;
        commaPos = commandNamesStr.indexOf(',', startPos);
    }
    
    // 处理最后一个指令名称
    if (startPos < commandNamesStr.length() && commandNamesCount < MAX_LEARN_COMMANDS) {
        String cmdName = commandNamesStr.substring(startPos);
        cmdName.trim();
        if (cmdName.length() > 0 && cmdName.length() < MAX_COMMAND_NAME_LEN) {
            const char* cmdNameStr = cmdName.c_str();
            if (cmdNameStr != nullptr) {
                strncpy(commandNamesList[commandNamesCount], cmdNameStr, MAX_COMMAND_NAME_LEN - 1);
                commandNamesList[commandNamesCount][MAX_COMMAND_NAME_LEN - 1] = '\0';
                commandNamesCount++;
            }
        }
    }
}

void IRLearner::freeLearnedDevice() {
    if (learnedDevice != nullptr) {
        // ⭐ commands 是 v1.3.41 起单独 new[] 出来的（按实际条数）。可能是 nullptr ——
        //   `delete[] nullptr` 是合法的，但 early return 路径上必须靠它把已分配的那半块收回来。
        delete[] learnedDevice->commands;
        learnedDevice->commands = nullptr;
        learnedDevice->commandCapacity = 0;
        delete learnedDevice;
        learnedDevice = nullptr;
    }
}

// ── 学习结果落盘（v1.3.46）──────────────────────────────────────────────────
//
// ⭐⭐ 为什么非做不可：学 2 条时序码会让空闲堆从 ~8 KB 永降到 ~4.5 KB，而 4.5 KB 已经低到
//   SDK 在**信标路径**上分配失败 ⇒ 学完 3~4 分钟设备**自行** `Exception (29)` 重启
//   （`epc1=0x4000df64` ROM memcpy、`ctx: sys`，栈里没有一帧在我们的代码里）。
//   结果只在 RAM ⇒ 重启即清空 —— 用户看到的就是"学完不显示、等半天、最后啥也没有"。
//   落盘后：① 那块 ~3.2 KB 立刻归还 ⇒ 堆回到 ~7.8 KB（不再崩）；
//          ② 重启/断电/OTA 都不再丢结果；③ 不用赶那"3 分钟内必须处理完"。
//
// ⚠️ 写盘时机就是"学习结束"（本函数只从 stopDeviceLearn 调用），此刻堆还是 ~5.9 KB
//   （台阶还没被后续请求叠加），比"事后读"时（~4.5 KB）宽裕。
// ⛔ 失败时不做任何释放，保持老行为 —— 宁可不省内存，也不能把结果弄丢。
// 前置声明：定义在 `convertToStringFormat` 上方（文件后半），
// 因为落盘路径（persistLearnedResult，在本行之后不远）与它共用同一套装配逻辑。
static size_t estimateLearnedDocSize(const LearnedDevice* dev);
static bool   fillLearnedDocString(const LearnedDevice* dev, DynamicJsonDocument& doc);

bool IRLearner::persistLearnedResult() {
    if (learnedDevice == nullptr) {
        return false;
    }

    // ⭐ 一条都没学到（用户中途点「停止学习」、或一条都没按就超时）⇒ 没有任何东西值得保留，
    //   但**必须把那块内存还回去**：否则它会一直占着，直到下次开始学习或重启 ——
    //   而空闲堆正好停在本函数开头说的"会被 SDK 抽干"的那一档。
    if (!learnedDevice->valid || learnedDevice->commandCount <= 0) {
        Serial.println(F("[IRLearner] 本次学习没有有效指令，直接释放缓冲区"));
        freeLearnedDevice();
        lastLearnedSignal.clear();
        hasLastSignal = false;
        return false;
    }

    // ⭐⭐ v1.3.47：**不再先在 RAM 里拼整份 String**。
    //   旧实现走 convertToStringFormat()：它自己要一块 JsonDocument 池（2 条 170 点的命令
    //   ≈ 1560 B）**再加**一份等长的 output String（reserve ≈ 1176 B），峰值 ~2.7 KB 连续块
    //   —— 而"刚学完"正是堆最紧的时刻。2026-10-04 实机（v1.3.46）坐实：
    //     `convertToStringFormat: Insufficient memory (free: 2720, max block: 1296, required: 2078)`
    //   ⇒ 落盘失败 ⇒ 缓冲不释放 ⇒ 45 秒后 `Exception (29)` ⇒ 用户"学完还是不显示记录"。
    //   改成把 doc **直接序列化进文件**（Storage::saveJsonAtomic），峰值只剩 doc 池那一块。
    //   ⚠️ 门槛也因此换了判据：不再问 checkMemory(1024)，而是"doc 池 + 512 的连续块"。
    const size_t docSize = estimateLearnedDocSize(learnedDevice);
    const size_t need = docSize + 512;   // 序列化期间键/值的临时开销
    if (ESP.getMaxFreeBlockSize() < need) {
        Serial.printf_P(PSTR("[IRLearner] 学习结果未能落盘：连续块不足 heap=%u blk=%u need=%u\n"),
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize(), (unsigned)need);
        return false;
    }

    const int count = learnedDevice->commandCount;
    {
        // doc 关在这个作用域里：写完盘立刻析构，后面做 json 变体时它已经不在 ——
        // 两块池叠在一起正是"刚学完"最容易失败的那个组合。
        DynamicJsonDocument doc(docSize);
        if (!fillLearnedDocString(learnedDevice, doc) || doc.overflowed()) {
            Serial.printf_P(PSTR("[IRLearner] 学习结果未能落盘：装配失败 heap=%u blk=%u size=%u\n"),
                          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize(),
                          (unsigned)docSize);
            return false;
        }
        if (!Storage::saveJsonAtomic(CONFIG_FILE_LEARNED, doc)) {
            // 写失败就把这份丢掉，RAM 里的结果保持不动（没落盘时前端还能走"即时转换"的老路径）
            Serial.printf_P(PSTR("[IRLearner] 学习结果未能落盘：写文件失败 heap=%u\n"),
                          (unsigned)ESP.getFreeHeap());
            return false;
        }
        // 条数标记（几字节）—— 重启后靠它知道"学过几条"，见 restorePersistedCount()。
        // 失败只是让"重启后读不到"（这次仍能读到），所以只报错不中断。
        if (!Storage::saveFileAtomic(CONFIG_FILE_LEARNED_COUNT, String(count))) {
            Serial.println(F("[IRLearner] 条数标记未能落盘 ⇒ 重启后读不到这次的结果"));
        }
    }

    // json 变体（commands 的值是对象）：尽力而为。面板从不请求它（只用 string），
    // 只有 MQTT 的 `query_learn_result` 带 format=json 时才读；写不出来只影响那一个接口。
    // 走老路径（它自带溢出重试），失败绝不影响上面已经写好的 string 版本。
    uint32_t jsonLen = 0;
    {
        String outJson = convertToJSONFormat();
        if (outJson.length() > 0 && Storage::saveFileAtomic(CONFIG_FILE_LEARNED_JSON, outJson)) {
            jsonLen = outJson.length();
        } else {
            Serial.println(F("[IRLearner] json 变体未能落盘（不影响面板）"));
        }
    }

    persistedCommandCount = count;
    freeLearnedDevice();          // ⭐ 关键：把那块还给堆（学 2 条时序码 ≈ 1.8 KB）
    lastLearnedSignal.clear();
    hasLastSignal = false;

    Serial.printf_P(PSTR("[IRLearner] 学习结果已落盘：%d 条 / 文档 %u B / json %u B，释放后 heap=%u blk=%u\n"),
                  count, (unsigned)docSize, (unsigned)jsonLen,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize());
    return true;
}

bool IRLearner::isResultPersisted() const {
    // ⚠️ 不看文件在不在，只看这个计数 —— 它同时表达"结果已落盘"与"条数"，
    //   两边不一致只可能出现在 flash 被人为破坏时，那时读出来是空串，按 CONVERT_FAILED 报。
    return persistedCommandCount > 0;
}

bool IRLearner::clearPersistedResult() {
    // 学习进行中不许清：清了之后 processDeviceLearnSignal 还会往 learnedDevice 里写，
    // 结果是"界面说已清除、实际又能读出一半"，比不做还糟。
    if (mode != LEARN_NONE) {
        return false;
    }

    bool removed = false;
    // 三个文件必须一起删 —— restorePersistedCount() 的判据是"计数与内容成对"，
    // 只删一半会让重启后的判定落进"有计数没内容"的兜底分支（虽然也算没有结果，
    // 但那是靠兜底救的，不该依赖）。
    if (Storage::fileExists(CONFIG_FILE_LEARNED)) {
        removed |= Storage::deleteFile(CONFIG_FILE_LEARNED);
    }
    if (Storage::fileExists(CONFIG_FILE_LEARNED_JSON)) {
        removed |= Storage::deleteFile(CONFIG_FILE_LEARNED_JSON);
    }
    if (Storage::fileExists(CONFIG_FILE_LEARNED_COUNT)) {
        removed |= Storage::deleteFile(CONFIG_FILE_LEARNED_COUNT);
    }
    if (persistedCommandCount > 0) {
        removed = true;
    }
    persistedCommandCount = 0;
    // RAM 里若还留着没落盘的会话，一并清掉（"清除"的语义就是什么都没有）
    freeLearnedDevice();
    lastLearnedSignal.clear();
    hasLastSignal = false;

    // v1.3.51：格式串放 flash（本 core 的 .rodata 在 DRAM，中文串尤其肥）
    Serial.printf_P(PSTR("[IRLearner] 学习结果已清除（%s）\n"),
                    removed ? "删掉了文件" : "本来就没有");
    return removed;
}

String IRLearner::loadPersistedResult(bool jsonFormat) const {
    if (persistedCommandCount <= 0) {
        return String();
    }
    // 对外格式与 convertToStringFormat()/convertToJSONFormat() 的输出**逐字相同**
    // （就是落盘时写进去的那份），所以 web_server / MQTT 的取法一行都不用改。
    return Storage::readFile(jsonFormat ? CONFIG_FILE_LEARNED_JSON : CONFIG_FILE_LEARNED);
}

// 辅助函数：将信号序列转换为字符串序列格式
static String sequenceToStringFormat(const SignalSequence& sequence) {
    String result;
    result.reserve(256);
    
    bool firstSignal = true;
    bool hasFirstInterval = (sequence.getIntervalCount() == sequence.signalCount);
    
    // 如果有第一个信号前的间隔
    if (hasFirstInterval && sequence.intervals[0] > 0) {
        result += "@";
        result += String(sequence.intervals[0]);
        result += ";";
    }
    
    // 遍历序列中的每个信号
    for (uint8_t j = 0; j < sequence.signalCount; j++) {
        const UnifiedSignal& signal = sequence.signals[j];
        
        if (!firstSignal) {
            result += ";";
        }
        firstSignal = false;
        
        // 转换信号为字符串格式
        if (signal.useTiming && signal.hasTiming()) {
            // 时序码信号：Base64格式
            result += signal.timingBase64;
        } else if (signal.isProtocol()) {
            // 协议信号：protocol,data格式
            result += String(signal.protocol);
            result += ",0x";
            char hexStr[9];
            snprintf(hexStr, sizeof(hexStr), "%08X", signal.data);
            result += hexStr;
        } else {
            // 无法转换，跳过
            continue;
        }
        
        // 添加间隔（如果有）
        uint16_t interval = 0;
        if (hasFirstInterval) {
            if (j < sequence.signalCount - 1) {
                interval = sequence.intervals[j + 1];
            }
        } else {
            if (j < sequence.signalCount - 1) {
                interval = sequence.intervals[j];
            }
        }
        
        if (interval > 0) {
            result += "@";
            result += String(interval);
        }
    }
    
    // 添加序列级别的repeatCount（如果>1）
    uint8_t seqRepeatCount = 1;
    if (sequence.signalCount > 0 && sequence.signals[0].repeatCount > 1) {
        seqRepeatCount = sequence.signals[0].repeatCount;
    }
    
    if (seqRepeatCount > 1) {
        result += "|";
        result += String(seqRepeatCount);
    }
    
    return result;
}

// 估算一条序列转成字符串后的长度（**不实际构造字符串**，纯算术）。
// 用途：给 convertToStringFormat 的 JsonDocument 定一个贴近实际的大小，
// 免得"估小了 → 溢出 → 重建双倍文档"发生在刚学完、内存最紧的时刻。
// 逻辑与 sequenceToStringFormat 逐句对齐（分隔符何时加、间隔何时附、repeat 后缀）。
static size_t estimateSequenceStringLength(const SignalSequence& sequence) {
    size_t len = 0;
    bool firstSignal = true;
    const bool hasFirstInterval = (sequence.getIntervalCount() == sequence.signalCount);

    // 第一个信号之前的间隔：'@' + 最多5位数 + ';'
    if (hasFirstInterval && sequence.getIntervalCount() > 0 && sequence.intervals[0] > 0) {
        len += 7;
    }

    for (uint8_t j = 0; j < sequence.signalCount; j++) {
        const UnifiedSignal& signal = sequence.signals[j];
        if (!firstSignal) len += 1;   // ';'
        firstSignal = false;

        if (signal.useTiming && signal.hasTiming()) {
            len += signal.timingBase64.length();   // Base64：点数 × 8/3
        } else if (signal.isProtocol()) {
            len += 15;    // 协议号(≤3) + ",0x" + 8位十六进制
        } else {
            continue;     // 不可转换，跳过（与 sequenceToStringFormat 一致）
        }

        uint16_t interval = 0;
        if (hasFirstInterval) {
            if (j < sequence.signalCount - 1) interval = sequence.intervals[j + 1];
        } else {
            if (j < sequence.signalCount - 1) interval = sequence.intervals[j];
        }
        if (interval > 0) len += 6;   // '@' + 最多5位数
    }

    if (sequence.signalCount > 0 && sequence.signals[0].repeatCount > 1) {
        len += 4;   // '|' + 最多3位数
    }
    return len;
}

// ── 学习结果的 JSON 装配（v1.3.47 从 convertToStringFormat 里抽出）─────────────
// 两个调用方共用同一份"算多大 / 填什么"，免得两处漂移：
//   ① convertToStringFormat()   —— 老路径：序列化成 String 交回调用方；
//   ② persistLearnedResult()    —— 新路径：serializeJson 直接写文件。
// ⚠️ 拆成两个函数而不是一个，是因为"序列化到哪"由调用方决定，而 doc 必须现造现用。

// 内容字符数（不含键名与 JSON 标点）。两个地方要它：
//   ① estimateLearnedDocSize() 定 doc 池大小；② convertToStringFormat() 定 output.reserve。
static size_t estimateLearnedContentLength(const LearnedDevice* dev) {
    size_t contentEstimate = 0;
    for (int i = 0; i < dev->commandCount && i < dev->commandCapacity; i++) {
        const LearnedCommand& c = dev->commands[i];
        if (!c.valid || c.sequence.signalCount == 0) continue;
        contentEstimate += estimateSequenceStringLength(c.sequence);
        contentEstimate += strlen((c.name[0] != '\0') ? c.name : "") + 1;
    }
    return contentEstimate;
}

// doc 池 = 内容 + 键拷贝 + 每个节点的槽位开销 + 对齐余量；一次给足。
static size_t estimateLearnedDocSize(const LearnedDevice* dev) {
    size_t contentEstimate = estimateLearnedContentLength(dev);
    size_t estimatedSize = 384 + contentEstimate + (size_t)dev->commandCount * 128;
    if (estimatedSize > 6144) estimatedSize = 6144;  // 上限（超过就交给调用方的重试/放弃）
    if (estimatedSize < 512) estimatedSize = 512;    // 下限
    return estimatedSize;
}

// 填成 SmartIR 风格的文档（commands 的值是序列字符串）。
// 返回 false = 一条有效命令都没填进去（调用方应当判失败，别写出一个空壳 JSON）。
static bool fillLearnedDocString(const LearnedDevice* dev, DynamicJsonDocument& doc) {
    int filled = 0;
    doc["manufacturer"] = "Learned";
    JsonArray models = doc.createNestedArray("supportedModels");
    models.add("Learned Device");
    doc["commandsEncoding"] = "Raw";
    doc["supportedController"] = "MQTT";

    JsonObject commands = doc.createNestedObject("commands");
    for (int i = 0; i < dev->commandCount && i < dev->commandCapacity; i++) {
        const LearnedCommand& cmd = dev->commands[i];
        if (!cmd.valid || cmd.sequence.signalCount == 0) {
            continue;
        }
        // 检查内存：如果内存不足，停止处理
        if (ESP.getFreeHeap() < 1500) {
            break;
        }
        String sequenceStr = sequenceToStringFormat(cmd.sequence);
        if (sequenceStr.length() > 0) {
            const char* cmdName = (cmd.name[0] != '\0') ? cmd.name : "";
            commands[cmdName] = sequenceStr;
            filled++;
        }
    }
    return filled > 0;
}

String IRLearner::convertToStringFormat() {
    if (learnedDevice == nullptr || !learnedDevice->valid || learnedDevice->commandCount == 0) {
        return "{}";
    }
    
    // 检查内存：如果内存不足，提前返回错误
    size_t freeHeap = ESP.getFreeHeap();
    size_t maxBlock = ESP.getMaxFreeBlockSize();
    
    // 估算JSON大小：按**实际内容**算（v1.3.29 修正）。
    // ⚠️ 旧的 `256 + 命令数*256` 严重低估：一条时序码的 Base64 长度 = 点数 × 8/3，
    //   144 点就是 384 字符（串口实测 `Timing:144 points`），两条命令的内容
    //   就顶穿 768 字节的估量 ⇒ **每次都必然溢出** ⇒ 在"刚学完、内存最紧"的
    //   时刻重建一份双倍文档。串口日志坐实过这条：
    //     `[IRLearner: convertToStringFormat] JSON文档溢出，尝试使用更大的文档`
    //   溢出后能不能拿到完整结果，取决于当时 freeHeap 是否 > 3.5 KB —— 于是
    //   同一个操作时好时坏。这里把估算做实，让它一次就装得下。
    // doc 池 = 内容 + 键拷贝 + 每个节点的槽位开销 + 对齐余量；一次给足。
    // ⚠️ v1.3.47：估算与装配都搬去了文件顶部的 estimateLearnedDocSize / fillLearnedDocString，
    //   与 persistLearnedResult() 的写盘路径共用同一份 —— 免得两处漂移。
    //   `contentEstimate` 单独取一次：待会儿给 output.reserve 用（不能拿池大小去 reserve）。
    size_t contentEstimate = estimateLearnedContentLength(learnedDevice);
    size_t estimatedSize = estimateLearnedDocSize(learnedDevice);
    
    // 检查最大连续块是否足够
    size_t requiredMemory = estimatedSize + 512;  // 额外512字节用于字符串操作
    if (freeHeap < 1500 || maxBlock < requiredMemory) {
        Serial.printf_P(PSTR("[IRLearner] convertToStringFormat: Insufficient memory (free: %d bytes, max block: %d bytes, required: %d bytes)\n"), 
                     freeHeap, maxBlock, requiredMemory);
        return "{}";
    }
    
    DynamicJsonDocument doc(estimatedSize);
    
    // SmartIR JSON配置文件格式（commands 的值是序列字符串）。装配逻辑与
    // persistLearnedResult() 的写盘路径共用同一份，见 fillLearnedDocString。
    if (!fillLearnedDocString(learnedDevice, doc)) {
        return "{}";
    }
    
    // 检查文档是否溢出
    if (doc.overflowed()) {
        char buffer[192];
        snprintf(buffer, sizeof(buffer), "[IRLearner: convertToStringFormat] JSON文档溢出，尝试使用更大的文档 (当前大小: %d bytes, 命令数: %d)", estimatedSize, learnedDevice->commandCount);
        logger.log("warning", buffer);
        
        // 尝试使用更大的文档重新构建
        size_t largerSize = estimatedSize * 2;
        // 除了大小上限，还必须在申请前确认当时真的拿得出这么多连续内存。
        // 溢出本身就说明内容偏大，而设备内存最紧张时这一点最不巧；只看上限就去
        // 申请，结果是重试失败 + 堆被搅碎。这是一条"整理学习结果"的辅助路径，
        // 不值得把堆打穿。
        if (largerSize <= 4096 && ESP.getFreeHeap() > largerSize + 2048) {
            DynamicJsonDocument largerDoc(largerSize);
            largerDoc["manufacturer"] = "Learned";
            JsonArray largerModels = largerDoc.createNestedArray("supportedModels");
            largerModels.add("Learned Device");
            largerDoc["commandsEncoding"] = "Raw";
            largerDoc["supportedController"] = "MQTT";
            JsonObject largerCommands = largerDoc.createNestedObject("commands");
            
            for (int i = 0; i < learnedDevice->commandCount && i < learnedDevice->commandCapacity; i++) {
                const LearnedCommand& cmd = learnedDevice->commands[i];
                if (!cmd.valid || cmd.sequence.signalCount == 0) continue;
                if (ESP.getFreeHeap() < 1500) break;
                
                String sequenceStr = sequenceToStringFormat(cmd.sequence);
                if (sequenceStr.length() > 0) {
                    const char* cmdName = (cmd.name[0] != '\0') ? cmd.name : "";
                    largerCommands[cmdName] = sequenceStr;
                }
            }
            
            if (!largerDoc.overflowed()) {
                // 拷贝赋值会深拷贝进一个新申请的等大内存池（峰值约 4~5 倍）；
                // move 只转移池指针，不再申请、不再抄。详见 logger.cpp 同一处的注释。
                doc = std::move(largerDoc);
            } else {
                char errorBuffer[192];
                snprintf(errorBuffer, sizeof(errorBuffer), "[IRLearner: convertToStringFormat] 更大的文档仍然溢出 (大小: %d bytes, 命令数: %d)", largerSize, learnedDevice->commandCount);
                logger.log("error", errorBuffer);
            }
        } else {
            char errorBuffer[192];
            snprintf(errorBuffer, sizeof(errorBuffer), "[IRLearner: convertToStringFormat] JSON文档过大，无法重试 (需要: %d bytes, 命令数: %d)", largerSize, learnedDevice->commandCount);
            logger.log("error", errorBuffer);
        }
    }
    
    // 序列化为字符串
    // ⚠️ reserve 只按**实际会写出的字符数**给（内容 + 键名 + JSON 标点 ≈ 内容 + 256），
    //   不要用 doc 的池大小 —— 那会在 doc 还活着时再要一块几乎等大的，
    //   瞬时峰值 = 池 + reserve，正是"刚学完内存最紧"时最容易失败的组合。
    String output;
    output.reserve(contentEstimate + 256);
    serializeJson(doc, output);
    
    // 如果仍然溢出，记录错误
    if (doc.overflowed()) {
        char buffer[192];
        snprintf(buffer, sizeof(buffer), "[IRLearner: convertToStringFormat] JSON文档溢出，无法序列化 (大小: %d bytes, 命令数: %d)", estimatedSize, learnedDevice->commandCount);
        logger.log("error", buffer);
    }
    
    return output;
}

String IRLearner::convertToJSONFormat() {
    if (learnedDevice == nullptr || !learnedDevice->valid || learnedDevice->commandCount == 0) {
        return "{}";
    }
    
    // 检查内存：如果内存不足，提前返回错误
    size_t freeHeap = ESP.getFreeHeap();
    size_t maxBlock = ESP.getMaxFreeBlockSize();
    
    // 估算JSON大小：基础结构 + 每个命令的序列JSON对象
    // 基础结构约200字节，每个命令的序列JSON对象平均200-400字节
    size_t estimatedSize = 512 + (learnedDevice->commandCount * 512);
    if (estimatedSize > 2048) estimatedSize = 2048;  // 限制最大2KB
    if (estimatedSize < 1024) estimatedSize = 1024;  // 至少1KB
    
    // 检查最大连续块是否足够
    size_t requiredMemory = estimatedSize + 512;  // 额外512字节用于字符串操作
    if (freeHeap < 1500 || maxBlock < requiredMemory) {
        Serial.printf_P(PSTR("[IRLearner] convertToJSONFormat: Insufficient memory (free: %d bytes, max block: %d bytes, required: %d bytes)\n"), 
                     freeHeap, maxBlock, requiredMemory);
        return "{}";
    }
    
    DynamicJsonDocument doc(estimatedSize);
    
    // JSON格式（与SmartIR不兼容）
    doc["manufacturer"] = "Learned";
    JsonArray models = doc.createNestedArray("supportedModels");
    models.add("Learned Device");
    doc["commandsEncoding"] = "Raw";
    doc["supportedController"] = "MQTT";
    
    // 创建commands对象
    JsonObject commands = doc.createNestedObject("commands");
    
    // 遍历所有命令，转换为JSON对象（序列格式）
    for (int i = 0; i < learnedDevice->commandCount && i < learnedDevice->commandCapacity; i++) {
        const LearnedCommand& cmd = learnedDevice->commands[i];
        if (!cmd.valid || cmd.sequence.signalCount == 0) {
            continue;
        }
        
        // 检查内存：如果内存不足，停止处理
        if (ESP.getFreeHeap() < 1500) {
            break;
        }
        
        // 输出为JSON对象（序列格式）
        const char* cmdName = (cmd.name[0] != '\0') ? cmd.name : "";
        JsonObject seqObj = commands.createNestedObject(cmdName);
        SignalUtils::sequenceToJSON(cmd.sequence, seqObj, true);
    }
    
    // 检查文档是否溢出
    if (doc.overflowed()) {
        char buffer[192];
        snprintf(buffer, sizeof(buffer), "[IRLearner: convertToJSONFormat] JSON文档溢出，尝试使用更大的文档 (当前大小: %d bytes, 命令数: %d)", estimatedSize, learnedDevice->commandCount);
        logger.log("warning", buffer);
        
        // 尝试使用更大的文档重新构建
        size_t largerSize = estimatedSize * 2;
        // 除了大小上限，还必须在申请前确认当时真的拿得出这么多连续内存。
        // 溢出本身就说明内容偏大，而设备内存最紧张时这一点最不巧；只看上限就去
        // 申请，结果是重试失败 + 堆被搅碎。这是一条"整理学习结果"的辅助路径，
        // 不值得把堆打穿。
        if (largerSize <= 4096 && ESP.getFreeHeap() > largerSize + 2048) {
            DynamicJsonDocument largerDoc(largerSize);
            largerDoc["manufacturer"] = "Learned";
            JsonArray largerModels = largerDoc.createNestedArray("supportedModels");
            largerModels.add("Learned Device");
            largerDoc["commandsEncoding"] = "Raw";
            largerDoc["supportedController"] = "MQTT";
            JsonObject largerCommands = largerDoc.createNestedObject("commands");
            
            for (int i = 0; i < learnedDevice->commandCount && i < learnedDevice->commandCapacity; i++) {
                const LearnedCommand& cmd = learnedDevice->commands[i];
                if (!cmd.valid || cmd.sequence.signalCount == 0) continue;
                if (ESP.getFreeHeap() < 1500) break;
                
                const char* cmdName = (cmd.name[0] != '\0') ? cmd.name : "";
                JsonObject seqObj = largerCommands.createNestedObject(cmdName);
                SignalUtils::sequenceToJSON(cmd.sequence, seqObj, true);
            }
            
            if (!largerDoc.overflowed()) {
                // 拷贝赋值会深拷贝进一个新申请的等大内存池（峰值约 4~5 倍）；
                // move 只转移池指针，不再申请、不再抄。详见 logger.cpp 同一处的注释。
                doc = std::move(largerDoc);
            } else {
                char errorBuffer[192];
                snprintf(errorBuffer, sizeof(errorBuffer), "[IRLearner: convertToJSONFormat] 更大的文档仍然溢出 (大小: %d bytes, 命令数: %d)", largerSize, learnedDevice->commandCount);
                logger.log("error", errorBuffer);
            }
        } else {
            char errorBuffer[192];
            snprintf(errorBuffer, sizeof(errorBuffer), "[IRLearner: convertToJSONFormat] JSON文档过大，无法重试 (需要: %d bytes, 命令数: %d)", largerSize, learnedDevice->commandCount);
            logger.log("error", errorBuffer);
        }
    }
    
    // 序列化为字符串
    String output;
    output.reserve(estimatedSize);  // 预分配内存
    serializeJson(doc, output);
    
    // 如果仍然溢出，记录错误
    if (doc.overflowed()) {
        char buffer[192];
        snprintf(buffer, sizeof(buffer), "[IRLearner: convertToJSONFormat] JSON文档溢出，无法序列化 (大小: %d bytes, 命令数: %d)", estimatedSize, learnedDevice->commandCount);
        logger.log("error", buffer);
    }
    
    return output;
}


// Base64解码 - 委托给 SignalUtils::base64ToTiming()
bool IRLearner::decodeBase64ToTiming(const String& base64, IRTiming* timing, int& count, int maxCount) {
    return SignalUtils::base64ToTiming(base64, timing, count, maxCount);
}

IRLearner::SignalFormat IRLearner::detectSignalFormat(const String& payload) {
    if (payload.length() == 0) {
        return FORMAT_BASE64;  // 默认
    }
    
    String trimmed = payload;
    trimmed.trim();
    
    // 检测是否为JSON格式（UnifiedSignal对象格式）
    if (trimmed.startsWith("{") && trimmed.endsWith("}")) {
        return FORMAT_SIGNALID;
    }
    
    // 检测是否为数组格式
    if (trimmed.startsWith("[") && trimmed.endsWith("]")) {
        return FORMAT_ARRAY;
    }
    
    // 检测是否为协议格式（包含逗号且是数字开头）
    if (trimmed.indexOf(',') > 0) {
        // 检查第一个字符是否为数字
        char firstChar = trimmed.charAt(0);
        if (firstChar >= '0' && firstChar <= '9') {
            return FORMAT_PROTOCOL;
        }
    }
    
    // 默认按Base64处理
    return FORMAT_BASE64;
}

bool IRLearner::parseArrayString(const String& arrayStr, IRTiming* timing, int& count, int maxCount) {
    if (arrayStr.length() == 0 || timing == nullptr || maxCount <= 0) {
        count = 0;
        return false;
    }
    
    String trimmed = arrayStr;
    trimmed.trim();
    
    // 移除方括号
    if (trimmed.startsWith("[")) {
        trimmed = trimmed.substring(1);
    }
    if (trimmed.endsWith("]")) {
        trimmed = trimmed.substring(0, trimmed.length() - 1);
    }
    
    // 解析逗号分隔的数字
    count = 0;
    int startPos = 0;
    bool level = true;  // 从高电平开始
    
    while (startPos < trimmed.length() && count < maxCount) {
        int commaPos = trimmed.indexOf(',', startPos);
        if (commaPos < 0) {
            commaPos = trimmed.length();
        }
        
        String numStr = trimmed.substring(startPos, commaPos);
        numStr.trim();
        
        if (numStr.length() > 0) {
            long duration = numStr.toInt();
            if (duration < 0) {
                duration = -duration;
                level = false;
            } else {
                level = true;
            }
            
            if (duration > 0 && duration < 1000000) {  // 合理的时序范围
                timing[count].duration = duration;
                timing[count].level = level;
                count++;
            }
        }
        
        startPos = commaPos + 1;
    }
    
    return (count > 0);
}

bool IRLearner::parseProtocolString(const String& protocolStr, UnifiedSignal& signal, uint8_t& repeatCount) {
    if (protocolStr.length() == 0) {
        return false;
    }
    
    String trimmed = protocolStr;
    trimmed.trim();
    
    // 按逗号分隔
    int comma1 = trimmed.indexOf(',');
    if (comma1 < 0) {
        return false;
    }
    
    // 检测逗号数量以判断格式
    int commaCount = 0;
    for (int i = 0; i < trimmed.length(); i++) {
        if (trimmed.charAt(i) == ',') {
            commaCount++;
        }
    }
    
    int comma2 = trimmed.indexOf(',', comma1 + 1);
    if (comma2 < 0) {
        // 只有两个参数：protocol,data（repeatCount默认为1）
        String protocolStr_part = trimmed.substring(0, comma1);
        String dataStr = trimmed.substring(comma1 + 1);
        
        protocolStr_part.trim();
        dataStr.trim();
        
        uint32_t protocol = protocolStr_part.toInt();
        uint32_t data = 0;
        
        // 解析十六进制数据
        if (dataStr.startsWith("0x") || dataStr.startsWith("0X")) {
            data = strtoul(dataStr.c_str(), nullptr, 16);
        } else {
            // 尝试十六进制（不带0x前缀）
            char* endPtr;
            data = strtoul(dataStr.c_str(), &endPtr, 16);
            // 如果解析失败或没有完全解析，尝试十进制
            if (*endPtr != '\0' || (data == 0 && dataStr.length() > 0 && dataStr.charAt(0) != '0')) {
                data = dataStr.toInt();
            }
        }
        
        signal.clear();
        signal.protocol = protocol;
        signal.data = data;
        signal.useTiming = false;  // 协议信号（protocol >= 1 && protocol <= 5 时）
        signal.repeatCount = 1;
        repeatCount = 1;
        
        return true;
    } else if (commaCount == 2) {
        // 三个参数：protocol,data,repeatCount（向后兼容）
        String protocolStr_part = trimmed.substring(0, comma1);
        String dataStr = trimmed.substring(comma1 + 1, comma2);
        String repeatStr = trimmed.substring(comma2 + 1);
        
        protocolStr_part.trim();
        dataStr.trim();
        repeatStr.trim();
        
        uint32_t protocol = protocolStr_part.toInt();
        uint32_t data = 0;
        
        // 解析十六进制数据
        if (dataStr.startsWith("0x") || dataStr.startsWith("0X")) {
            data = strtoul(dataStr.c_str(), nullptr, 16);
        } else {
            // 尝试十六进制（不带0x前缀）
            char* endPtr;
            data = strtoul(dataStr.c_str(), &endPtr, 16);
            // 如果解析失败或没有完全解析，尝试十进制
            if (*endPtr != '\0' || (data == 0 && dataStr.length() > 0 && dataStr.charAt(0) != '0')) {
                data = dataStr.toInt();
            }
        }
        
        uint8_t repeat = repeatStr.toInt();
        if (repeat == 0) repeat = 1;
        
        signal.clear();
        signal.protocol = protocol;
        signal.data = data;
        signal.useTiming = false;  // 协议信号（protocol >= 1 && protocol <= 5 时）
        signal.repeatCount = repeat;
        repeatCount = repeat;
        
        return true;
    } else if (commaCount == 4) {
        // 五个参数：protocol,data,hash,isProtocol,repeatCount（新格式）
        int comma3 = trimmed.indexOf(',', comma2 + 1);
        int comma4 = trimmed.indexOf(',', comma3 + 1);
        
        if (comma3 < 0 || comma4 < 0) {
            return false;
        }
        
        String protocolStr_part = trimmed.substring(0, comma1);
        String dataStr = trimmed.substring(comma1 + 1, comma2);
        String hashStr = trimmed.substring(comma2 + 1, comma3);
        String isProtocolStr = trimmed.substring(comma3 + 1, comma4);
        String repeatStr = trimmed.substring(comma4 + 1);
        
        protocolStr_part.trim();
        dataStr.trim();
        hashStr.trim();
        isProtocolStr.trim();
        repeatStr.trim();
        
        uint32_t protocol = protocolStr_part.toInt();
        uint32_t data = 0;
        
        // 解析data（十六进制）
        if (dataStr.startsWith("0x") || dataStr.startsWith("0X")) {
            data = strtoul(dataStr.c_str(), nullptr, 16);
        } else {
            char* endPtr;
            data = strtoul(dataStr.c_str(), &endPtr, 16);
            if (*endPtr != '\0' || (data == 0 && dataStr.length() > 0 && dataStr.charAt(0) != '0')) {
                data = dataStr.toInt();
            }
        }
        
        // hash字段已移除，不再解析（保留hashStr解析以兼容旧格式，但不使用）
        
        // 解析isProtocol（true/false或1/0，兼容旧格式）
        bool isProtocol = false;
        if (isProtocolStr == "true" || isProtocolStr == "1" || isProtocolStr == "True" || isProtocolStr == "TRUE") {
            isProtocol = true;
        }
        
        // 解析repeatCount
        uint8_t repeat = repeatStr.toInt();
        if (repeat == 0) repeat = 1;
        
        signal.clear();
        signal.protocol = protocol;
        signal.data = data;
        signal.useTiming = !isProtocol;  // 根据 isProtocol 设置 useTiming
        signal.repeatCount = repeat;
        repeatCount = repeat;
        
        return true;
    } else {
        // 不支持的参数数量
        return false;
    }
}

bool IRLearner::parseSignalPayload(const String& payload, UnifiedSignal& signal, IRTiming* timing, int& timingCount, int maxTimingCount, uint8_t& repeatCount, SignalFormat& format) {
    format = detectSignalFormat(payload);
    
    switch (format) {
        case FORMAT_ARRAY: {
            // 数组格式：支持|repeatCount后缀
            int pipePos = payload.lastIndexOf('|');
            if (pipePos > 0 && pipePos < payload.length() - 1) {
                // 有|后缀，解析repeatCount（使用atoi避免创建String对象）
                repeatCount = atoi(payload.c_str() + pipePos + 1);
                if (repeatCount == 0) repeatCount = 1;
                
                // 创建临时String用于数组解析
                String arrayPart = payload.substring(0, pipePos);
                if (!parseArrayString(arrayPart, timing, timingCount, maxTimingCount)) {
                    return false;
                }
            } else {
                // 无后缀，默认repeatCount=1
                if (!parseArrayString(payload, timing, timingCount, maxTimingCount)) {
                    return false;
                }
                repeatCount = 1;
            }
            return true;
        }
        
        case FORMAT_PROTOCOL: {
            // 协议格式
            if (!parseProtocolString(payload, signal, repeatCount)) {
                return false;
            }
            timingCount = 0;
            return true;
        }
        
        case FORMAT_BASE64: {
            // Base64格式：支持|repeatCount后缀
            int pipePos = payload.lastIndexOf('|');
            if (pipePos > 0 && pipePos < payload.length() - 1) {
                // 有|后缀，解析repeatCount（使用atoi避免创建String对象）
                repeatCount = atoi(payload.c_str() + pipePos + 1);
                if (repeatCount == 0) repeatCount = 1;
                
                // 创建临时String用于Base64解码
                String base64Part = payload.substring(0, pipePos);
                if (!decodeBase64ToTiming(base64Part, timing, timingCount, maxTimingCount)) {
                    return false;
                }
            } else {
                // 无后缀，默认repeatCount=1
                if (!decodeBase64ToTiming(payload, timing, timingCount, maxTimingCount)) {
                    return false;
                }
                repeatCount = 1;
            }
            return true;
        }
        
        case FORMAT_SIGNALID: {
            // UnifiedSignal对象格式（JSON），需要调用者解析JSON
            return false;  // 返回false，让调用者处理JSON解析
        }
        
        default:
            return false;
    }
}

bool IRLearner::parseStringSequence(const String& payload, SignalSequence& sequence, IRTiming* timingBuffer, int maxTimingCount) {
    if (payload.length() == 0 || timingBuffer == nullptr || maxTimingCount <= 0) {
        return false;
    }
    
    String trimmed = payload;
    trimmed.trim();
    
    if (trimmed.length() == 0) {
        return false;
    }
    
    sequence.clear();
    
    // 1. 先提取最后的|repeatCount（如果存在）
    uint8_t sequenceRepeatCount = 1;
    int pipePos = trimmed.lastIndexOf('|');
    if (pipePos > 0 && pipePos < trimmed.length() - 1) {
        // 有|后缀，解析repeatCount
        sequenceRepeatCount = atoi(trimmed.c_str() + pipePos + 1);
        if (sequenceRepeatCount == 0) sequenceRepeatCount = 1;
        if (sequenceRepeatCount > SIGNAL_REPEAT_MAX) sequenceRepeatCount = SIGNAL_REPEAT_MAX;
        
        // 移除|repeatCount部分
        trimmed = trimmed.substring(0, pipePos);
        trimmed.trim();
    }
    
    if (trimmed.length() == 0) {
        return false;
    }
    
    // 2. 检查第一个信号前是否有间隔（格式：@50;signal1@30;signal2）
    bool hasFirstInterval = false;
    uint16_t firstInterval = 0;
    int startPos = 0;
    
    if (trimmed.startsWith("@")) {
        // 第一个信号前有间隔
        hasFirstInterval = true;
        int firstSemicolon = trimmed.indexOf(';');
        if (firstSemicolon > 0) {
            // 提取@后面的数字（直到分号）
            String intervalStr = trimmed.substring(1, firstSemicolon);
            intervalStr.trim();
            firstInterval = intervalStr.toInt();
            
            // 从分号后开始解析信号
            startPos = firstSemicolon + 1;
        } else {
            // 格式错误：@50后面没有分号
            return false;
        }
    }
    
    // 3. 解析每个信号段（带 @ 前缀时，段尾 @ 表示「该段 IR 结束后的间隔」，写入 intervals[si+1]，与 intervals[0] 前导分开处理）
    uint16_t gapAfterSignal[MAX_B_SIGNALS_PER_RULE];
    for (int g = 0; g < MAX_B_SIGNALS_PER_RULE; g++) {
        gapAfterSignal[g] = 0;
    }
    
    int signalIndex = 0;
    int semicolonPos = trimmed.indexOf(';', startPos);
    
    while (startPos < trimmed.length() && signalIndex < MAX_B_SIGNALS_PER_RULE) {
        // 获取当前信号段
        String segment;
        if (semicolonPos >= 0) {
            segment = trimmed.substring(startPos, semicolonPos);
        } else {
            segment = trimmed.substring(startPos);
        }
        segment.trim();
        
        if (segment.length() == 0) {
            // 空段，跳过
            startPos = semicolonPos + 1;
            semicolonPos = trimmed.indexOf(';', startPos);
            continue;
        }
        
        // 提取@间隔（如果存在）
        uint16_t interval = 0;
        int atPos = segment.indexOf('@');
        String signalPart = segment;
        
        if (atPos > 0) {
            // 有@间隔
            String intervalStr = segment.substring(atPos + 1);
            intervalStr.trim();
            interval = intervalStr.toInt();
            signalPart = segment.substring(0, atPos);
            signalPart.trim();
        }
        
        if (signalPart.length() == 0) {
            // 只有间隔，没有信号，跳过
            startPos = semicolonPos + 1;
            semicolonPos = trimmed.indexOf(';', startPos);
            continue;
        }
        
        // 4. 解析信号部分（Base64/数组/协议格式）
        UnifiedSignal signal;
        signal.clear();
        signal.repeatCount = sequenceRepeatCount;  // 使用序列级别的repeatCount
        
        SignalFormat format = detectSignalFormat(signalPart);
        int timingCount = 0;
        uint8_t signalRepeatCount = 1;
        
        bool parseSuccess = false;
        
        switch (format) {
            case FORMAT_ARRAY: {
                // 数组格式
                if (parseArrayString(signalPart, timingBuffer, timingCount, maxTimingCount)) {
                    // ⚠️ timingToBase64() 在低堆时可能返回空串（它要 reserve 一份 ~540 B 的 String，
                    //   而 byteCount 超 400 还会直接返回 ""）。以前不检查就置 parseSuccess ⇒
                    //   信号变成“useTiming 但没有数据”，在 sendSequence() 里被静默跳过。
                    String b64 = SignalUtils::timingToBase64(timingBuffer, timingCount);
                    if (b64.length() > 0) {
                        signal.useTiming = true;
                        signal.timingBase64 = b64;
                        signal.timingCount = timingCount;
                        parseSuccess = true;
                    } else {
                        Serial.printf_P(PSTR("[IRLearner] 时序码编码失败(内存) pts=%d heap=%u\n"),
                                      timingCount, (unsigned)ESP.getFreeHeap());
                    }
                }
                break;
            }
            
            case FORMAT_PROTOCOL: {
                // 协议格式
                if (parseProtocolString(signalPart, signal, signalRepeatCount)) {
                    signal.repeatCount = signalRepeatCount;  // 使用信号自身的repeatCount
                    parseSuccess = true;
                }
                break;
            }
            
            case FORMAT_BASE64: {
                // Base64格式
                if (decodeBase64ToTiming(signalPart, timingBuffer, timingCount, maxTimingCount)) {
                    signal.useTiming = true;
                    // ⭐ v1.3.45：输入**本来就是 Base64**，直接复用，不再编码回去。
                    //   那一步要再分配一份 ~540 B 的 String，在低堆时可能失败并返回空串 ——
                    //   实测（2026-10-04，heap=3680）：200 点信号就是这么变成
                    //   “useTiming=true 但 timingBase64 为空”的，随后在 sendSequence() 里
                    //   被静默跳过，而界面显示“已发送”。省掉这一次分配后，该路径
                    //   在低堆下不再依赖 heap。
                    signal.timingBase64 = signalPart;
                    signal.timingCount = timingCount;
                    parseSuccess = (signal.timingBase64.length() > 0);
                }
                break;
            }
            
            default:
                parseSuccess = false;
                break;
        }
        
        if (!parseSuccess) {
            // 解析失败，跳过这个信号
            startPos = semicolonPos + 1;
            semicolonPos = trimmed.indexOf(';', startPos);
            continue;
        }
        
        if (hasFirstInterval) {
            gapAfterSignal[signalIndex] = interval;
        }
        
        // 5. 添加到序列
        // 带 @ 前缀时：段内间隔不交给 addSignal（否则会与 intervals[0] 前导语义冲突并覆盖错误下标），在循环结束后写入 intervals[]。
        uint16_t passInterval = interval;
        bool useFirstIntervalLayout = false;
        if (!hasFirstInterval) {
            useFirstIntervalLayout = false;
        } else {
            passInterval = 0;
            useFirstIntervalLayout = true;
        }
        if (!sequence.addSignal(signal, passInterval, useFirstIntervalLayout)) {
            // 序列已满
            break;
        }
        
        signalIndex++;
        
        // 移动到下一个信号段
        if (semicolonPos >= 0) {
            startPos = semicolonPos + 1;
            semicolonPos = trimmed.indexOf(';', startPos);
        } else {
            break;
        }
    }
    
    if (sequence.signalCount == 0) {
        return false;
    }
    
    if (hasFirstInterval) {
        memset(sequence.intervals, 0, sizeof(sequence.intervals));
        sequence.intervals[0] = firstInterval;
        for (int si = 0; si < sequence.signalCount && si < MAX_B_SIGNALS_PER_RULE; si++) {
            if (gapAfterSignal[si] > 0 && (si + 1) < MAX_B_SIGNALS_PER_RULE) {
                sequence.intervals[si + 1] = gapAfterSignal[si];
            }
        }
    }
    
    sequence.valid = true;
    return true;
}
