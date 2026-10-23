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
#include "utils/ota_service.h"
#include "utils/safe_mode.h"
#include "utils/health_guard.h"
#include "utils/net_watchdog.h"
#include "utils/device_state.h"
#include "utils/crash_record.h"   // 崩溃自留档（v1.3.50）—— 212 无串口，栈只能靠它留

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

Logger logger;
IRForwarder irForwarder(irMapper, irTransmitter, forwardQueue, logger);
IRLearner irLearner(irReceiver, irParser, logger);
HAMQTTHandler haHandler(mqttClient, irMapper, irForwarder, logger);

// OTA 无线升级服务（ArduinoOTA 主通道 + 浏览器 /update 备用通道）
OtaService otaService;

// 任务定时器（使用毫秒时间戳）
unsigned long lastIRReceiverTime = 0;
unsigned long lastIRTransmitterTime = 0;
unsigned long lastMQTTTime = 0;
unsigned long lastNetworkTime = 0;
unsigned long lastButtonCheckTime = 0;

// 任务间隔（毫秒）
const unsigned long IR_RECEIVER_INTERVAL = 10;    // 10ms
// ⚠️ 这个常量原来写的是 10，而下面真正用的是硬编码的 5 —— 改常量不生效，
// 是"看代码以为是 10、实测是 5"这类最容易白排查半天的坑。现在两边一致：5ms。
const unsigned long IR_TRANSMITTER_INTERVAL = 5;  // 5ms，提升转发响应速度
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
// 只影响"配了 MQTT 的设备"冷启动后的头几秒：先让 WiFi/MQTT/Discovery 把内存抢完，
// 之后再让 Web 服务上线。上线之后就一直开着，直到内存兜底把它请下去。
const unsigned long WEB_SERVER_START_DELAY = 8000; // 8秒延迟
unsigned long systemStartTime = 0;

// Web服务器启停判定。
// 旧逻辑是「MQTT 已连 + Discovery 已发布 ⇒ 停掉 Web 服务」，出发点省内存，
// 但结果是：设备越正常，面板越打不开 —— 而"设备正常"恰恰是用户想看面板的时刻。
// 现在只保留内存兜底：面板是产品的一部分，不是降级通道。
// outUsagePercent: 输出参数，返回当前内存使用率（避免调用方重复计算）
bool shouldWebServerRun(unsigned long now, bool wifiConnected, bool mqttConnected, bool mqttConfigured, float& outUsagePercent) {
    (void)mqttConnected;
    outUsagePercent = MemoryMonitor::getUsagePercent();
    unsigned long elapsedSinceStart = now - systemStartTime;

    // ── 内存兜底（唯一的关闭理由）────────────────────────────────────
    // 只按绝对堆判断，不按"使用率"（详见 constants.h 里那段说明）。
    static bool webStoppedByMemory = false;
    static unsigned long webStoppedAt = 0;
    static uint8_t lowMemoryStreak = 0;

    const size_t freeHeap = MemoryMonitor::getFreeHeap();
    const size_t maxBlock = MemoryMonitor::getLargestFreeBlock();
    const bool critical = (freeHeap < MEMORY_WEBSERVER_STOP_HEAP) ||
                          (maxBlock < MEMORY_WEBSERVER_MIN_BLOCK);

    // 只在"连续多次都低"时才当作真危险（见 constants.h 里 TRIP_CHECKS 的说明）
    if (critical) {
        if (lowMemoryStreak < 255) lowMemoryStreak++;
    } else {
        lowMemoryStreak = 0;
    }
    const bool memoryCrisis = (lowMemoryStreak >= MEMORY_WEBSERVER_TRIP_CHECKS);

    if (memoryCrisis) {
        if (!webStoppedByMemory) {
            webStoppedByMemory = true;
            webStoppedAt = now;
            // 用 Serial 直接打印（不走 DEBUG_PRINTF）——面板启停是最需要事后取证的
            // 事件，而 DEBUG_ENABLED 默认是关的，否则串口里什么都看不到。
            Serial.printf_P(PSTR("[面板] 内存见底，暂停面板 (空闲堆 %u B / 最大连续块 %u B)\n"),
                          (unsigned)freeHeap, (unsigned)maxBlock);
        }
    } else if (webStoppedByMemory) {
        // 恢复判据只看空闲堆。不再看"最大连续块"：堆碎片率实测能到 46%，
        // 该条件可能永远回不到阈值 ⇒ 面板永久锁死（用户只能断电重启）。
        const bool recovered = (freeHeap >= MEMORY_WEBSERVER_RESUME_HEAP);
        // 逃生通道：不管内存有没有回升，最多等 RETRY_MS 就强开一次，
        // 保证"面板打不开"不可能是永久状态。
        // ⚠️ 以前这里也要求 `freeHeap >= MEMORY_WEBSERVER_RESUME_HEAP`，于是
        // forceRetry 的条件**严格强于** recovered ⇒ `recovered || forceRetry`
        // 恒等于 recovered ⇒ 这条逃生通道从未生效过。而它本来就是为
        // "内存一直回不来"准备的（配了 MQTT 稳态只有 3~5 KB，堆停在
        // RESUME_HEAP 以下完全可能长期不回升）—— 恰恰是最需要的场景。
        const bool forceRetry = (now - webStoppedAt >= MEMORY_WEBSERVER_RETRY_MS);
        if (recovered || forceRetry) {
            webStoppedByMemory = false;
            webStoppedAt = 0;
            lowMemoryStreak = 0;
            Serial.printf_P(PSTR("[面板] 恢复面板（%s，空闲堆 %u B / 最大连续块 %u B）\n"),
                          recovered ? "内存回升" : "超时强制重试",
                          (unsigned)freeHeap, (unsigned)maxBlock);
        }
    }
    if (webStoppedByMemory) {
        return false;
    }
    
    // AP模式：内存充足时运行（配网必须能进）
    if (wifiManager.getStatus() == WIFI_AP_MODE) {
        return true;
    }
    
    // WiFi未连接：此时没人能访问，省下这点内存
    if (!wifiConnected) {
        return false;
    }

    // 配了 MQTT：先让出启动头几秒，避免和 MQTT 连接/Discovery 抢内存
    if (mqttConfigured && elapsedSinceStart < WEB_SERVER_START_DELAY) {
        return false;
    }

    return true;  // 联网即常驻
}

void setup() {
    // 尽早记录初始堆大小（在任何分配之前）
    size_t earlyFreeHeap = ESP.getFreeHeap();

    Serial.begin(115200);
    Serial.println(F("\n\n=== ORVIBO-CT30W 红外转发器启动 ==="));

    // 固件身份：OTA 之后靠它 + Sketch MD5 确认到底刷没刷上去
    SafeMode::printBanner();

    // 记下上次为什么重启（崩溃 / 看门狗 / 自己重启 / OTA），供 /api/status 上报
    HealthGuard::begin();

    // 网络僵死自愈（v1.3.36）—— 与 HealthGuard 分工：它管"堆见底"，本模块管
    // "主循环活着、WiFi 自认连着、但 LAN 上彻底消失"那种只能断电的死法。
    // 放在这里只是为了尽早读 RTC（不依赖文件系统）；真正开始探测要等 uptime 过保护期。
    NetWatchdog::begin();

    // 安全模式判定 —— 必须排在文件系统初始化（第 138 行附近）与按钮初始化之前。
    // 这两处之后的代码都可能因为固件 bug 崩掉，真崩了就轮不到这里救援。
    //
    // 传 `isAbnormalReset()` 进去：只有"上次崩溃/看门狗复位"或"上次没走完 setup()"才算失败。
    // 加"复位原因"这一条是为了覆盖"setup 走完、一进业务循环就崩"的坏固件（旧写法看不见它）。
    // 详见 safe_mode.h 顶部。
#ifdef SAFE_MODE_TEST
    // ------------------------------------------------------------------
    // 自测钩子：演练"坏固件 → 自动进安全模式 → 再从空中刷出来"
    // ------------------------------------------------------------------
    // 连调 3 次 `noteBoot(true)`（等价于连续 3 次崩溃复位）把计数推到阈值，
    // 用来在不制造真砖的前提下验证整条救援链路（能否进得去、进去后能否再 OTA 出来）。
    // 编法：`PLATFORMIO_BUILD_FLAGS="-DSAFE_MODE_TEST=1" pio run`（见 docs/flashing-and-ota.md §8.4）。
    // 正式固件不带这个宏，下面这 3 行不参与编译，行为完全不受影响。
    Serial.println(F("[自测] SAFE_MODE_TEST 编译：伪造 3 次崩溃复位，将自动进安全模式"));
    SafeMode::noteBoot(true);
    SafeMode::noteBoot(true);
    SafeMode::noteBoot(true);
#else
    SafeMode::noteBoot(HealthGuard::isAbnormalReset());
#endif
    if (SafeMode::shouldEnter()) {
        SafeMode::enter();   // 不返回
    }

    // 初始化内存监控（尽早初始化，传入启动时的堆大小）
    MemoryMonitor::init(earlyFreeHeap);
    
    // 初始化存储
    //
    // 🔴 这里**绝对不能**写成 `while(1) delay(1000)` 死等（v1.3.15 已改）：
    //    delay() 会喂看门狗 ⇒ 既不崩也不重启 ⇒ RTC 里的"可疑启动计数"永远不涨
    //    ⇒ 安全模式永远不会被触发 ⇒ 设备变成"空中无救"，只能拆壳接串口。
    //    而本项目的整条可靠性策略都建立在"任何坏状态都能收敛到安全模式再刷回来"之上。
    //    正确写法就是下面这样：重试几次 → 格式化 → 再失败就**主动重启**。
    //    重启会让计数 +1（因为本次没走完 setup()），连续 3 次自动进安全模式；
    //    安全模式允许文件系统挂不上（只读挂载意图、绝不格式化），照样能空中刷固件。
    if (!Storage::init()) {
        Serial.println(F("LittleFS初始化失败，重试..."));
        bool fsOk = false;
        for (uint8_t retry = 0; retry < STORAGE_INIT_RETRY && !fsOk; retry++) {
            delay(STORAGE_INIT_RETRY_DELAY_MS);
            fsOk = Storage::init();
            Serial.printf_P(PSTR("  第 %u 次重试: %s\n"), (unsigned)(retry + 1), fsOk ? "成功" : "仍失败");
        }

        if (!fsOk) {
            // 走到这里多半是文件系统区没格式化或已损坏。格式化会清掉 WiFi/MQTT 配置，
            // 但那只是"要重新配一次网"；不格式化的代价是设备永久不可用。两害相权取其轻。
            Serial.println(F("LittleFS 仍无法挂载，尝试格式化（会清空设备上的配置）..."));
            if (Storage::format() && Storage::init()) {
                fsOk = true;
                Serial.println(F("格式化后挂载成功 —— 配置已清空，请重新配网"));
            }
        }

        if (!fsOk) {
            Serial.println(F("[存储] 文件系统彻底不可用，主动重启让启动失败计数累积"));
            Serial.println(F("[存储] 连续 3 次即自动进入安全模式（那里允许文件系统挂不上）"));
            delay(200);
            ESP.restart();   // 不返回
        }
    }
    Serial.println(F("LittleFS初始化成功"));

    // 崩溃自留档（v1.3.50）：上一次若是崩的，现场已经在 RTC 里等着了，这里搬到 flash。
    // 必须排在文件系统之后（要落盘）、且**要早于**任何可能再次崩溃的业务初始化
    // —— 万一这次又崩了，上一份现场不能跟着丢。
    CrashRecord::collectOnBoot();

#ifdef CRASH_TEST
    // ------------------------------------------------------------------
    // 自测钩子：伪造一次"崩溃现场"（**不真的崩**），验证整条链路通不通
    // ------------------------------------------------------------------
    // 直接喂一份假现场给崩溃回调 → 立刻重启 → 下次开机应该能通过
    // `GET /api/diag/crash` 读到它，且 epc1 能被 tools/decode_crash.py 反解成 setup()。
    // 编法：`PLATFORMIO_BUILD_FLAGS="-DCRASH_TEST=1" pio run`（同 SAFE_MODE_TEST 的用法）。
    // 正式固件不带这个宏，下面整段不参与编译。**故意不真的制造崩溃**：真崩有可能
    // 触发"连续崩溃 → 安全模式"，为一次自测不值当。
    // ⚠️ **一发即止**：只在"还没有现场"时伪造，否则每次开机都重启 ⇒ 连崩 3 次
    //    会被 SafeMode 收走。第二次开机时 collectOnBoot() 已经把现场落盘了，
    //    这里的 `has()` 就是假，于是正常启动。
    if (!CrashRecord::has()) {
        Serial.println(F("[自测] CRASH_TEST 编译：伪造一份崩溃现场后重启（只此一次）"));
        uint32_t fakeStack[8];
        for (uint32_t i = 0; i < 8; i++) {
            fakeStack[i] = (uint32_t)(uintptr_t)&setup + i * 4;   // 指向真函数，便于验证反解
        }
        CrashRecord::captureForTest(2 /*REASON_EXCEPTION_RST*/, 29 /*load prohibited*/,
                                    (uint32_t)(uintptr_t)&setup, 0,
                                    (uint32_t)(uintptr_t)&setup, 1024,
                                    0x3ffffa00, fakeStack, 8);
        Serial.flush();
        ESP.restart();   // 不返回
    } else {
        Serial.println(F("[自测] CRASH_TEST：现场已在 flash 里，本次正常启动"));
    }
#endif

    // 设备级状态（转发总开关等）——必须在 IRForwarder 之前加载，否则开关状态会被默认值盖掉
    DeviceState::init();
    
    // 初始化日志
    if (!logger.init()) {
        Serial.println(F("日志系统初始化失败！"));
    }
    logger.log("system", "设备启动");
    
    // 初始化LED
    ledControl.init();
    ledControl.setState(LED_FAST_BLINK);
    
    // 初始化按钮处理器
    if (!buttonHandler.init()) {
        Serial.println(F("按钮处理器初始化失败！"));
    }
    
    // 设置长按回调函数（重置配置）
    buttonHandler.setLongPressCallback([]() {
        Serial.println(F("\n[按钮] 检测到长按，开始重置配置..."));
        
        // LED快速闪烁提示
        for (int i = 0; i < 10; i++) {
            ledControl.setState(LED_FAST_BLINK);
            delay(100);
            ledControl.setState(LED_OFF);
            delay(100);
        }
        
        // 清除所有配置
        networkConfig.reset();
        // 开关也回到默认"开"，否则重置完设备还停在"转发关"，会以为是重置没生效
        DeviceState::setForwardingEnabled(true);
        Serial.println(F("[按钮] 配置已清除"));
        
        // LED常亮1秒
        ledControl.setState(LED_ON);
        delay(1000);
        
        // 重启设备
        Serial.println(F("[按钮] 设备即将重启..."));
        delay(500);
        ESP.restart();
    });
    
    // 加载网络配置
    networkConfig.load();
    
    // 设置日志记录器到映射表管理器
    irMapper.setLogger(&logger);
    
    // 初始化映射表（在网络模块之前初始化，避免内存不足）
    if (!irMapper.init()) {
        Serial.println(F("映射表初始化失败！"));
    }
    
    // 如果映射表为空，初始化默认映射关系（在网络模块之前，确保有足够内存）
    // ⚠️ 只在"确实是空的"时候写默认规则。若上一次 loadFromFlash() 是
    // 「文件在、但读不出来」（内存不足 / JSON 文档溢出 / 打不开），此时
    // getRuleCount() 同样是 0，但文件里可能压着用户 20 条规则 —— 一写默认
    // 就不可逆地覆盖了。宁可空着跑，也不要替用户删数据。
    if (!irMapper.hadLoadFailure() && irMapper.getRuleCount() == 0) {
        Serial.println(F("映射表为空，正在初始化默认映射关系..."));
        if (irMapper.initDefaultMappings()) {
            Serial.println(F("默认映射关系初始化成功"));
            logger.log("system", "默认映射关系已初始化");
        } else {
            Serial.println(F("默认映射关系初始化失败"));
            logger.logError("默认映射关系初始化失败");
        }
    }
    
    // 初始化WiFi
    if (!wifiManager.init()) {
        Serial.println(F("WiFi初始化失败，进入AP模式"));
    }
    
    // 初始化MQTT
    mqttClient.init();
    
    // 初始化Web服务器（仅初始化路由，不启动）
    webServer.init();

    // OTA：ArduinoOTA 常驻（与 Web 服务是否被内存策略停掉无关），
    // 浏览器 /update 作为备用通道挂到同一个 Web 服务实例上
    otaService.begin(&webServer.httpServer());
    
    // 注入依赖到Web服务器
    webServer.setIRMapper(&irMapper);
    webServer.setIRForwarder(&irForwarder);
    webServer.setIRLearner(&irLearner);
    webServer.setLogger(&logger);
    webServer.setMQTTClient(&mqttClient);
    
    // 记录系统启动时间（用于Web服务器延迟启动）
    systemStartTime = millis();
    
    // AP模式下立即启动Web服务器（配网需要）
    if (wifiManager.getStatus() == WIFI_AP_MODE) {
        webServer.start();
    }
    
    // 初始化AT指令处理器
    atHandler.init();
    
    // 初始化硬件
    // 注意 GPIO5 的中断归属：工作模式下归 IRrecv（下面 irParser.init() 里的
    // enableIRIn()），IRReceiver::init() 只配引脚；只有进入学习模式时才由
    // IRLearner 调 attach() 切给自定义接收器，结束时 detach() 交还。
    if (!irReceiver.init()) {
        Serial.println(F("红外接收器初始化失败！"));
        logger.logError("红外接收器初始化失败");
    }
    
    if (!irTransmitter.init()) {
        Serial.println(F("红外发射器初始化失败！"));
        logger.logError("红外发射器初始化失败");
    }
    
    // 初始化红外核心
    if (!irParser.init()) {
        Serial.println(F("红外解析器初始化失败！"));
    }
    
    if (!irForwarder.init()) {
        Serial.println(F("转发器初始化失败！"));
    }
    // 恢复持久化的转发总开关（关着就关着，别一重启又自己开了）
    irForwarder.setEnabled(DeviceState::isForwardingEnabled());
    Serial.printf_P(PSTR("[转发] 总开关：%s\n"), irForwarder.isEnabled() ? "开" : "关");
    
    irLearner.init();
    
    // 初始化HA接口
    haHandler.init();
    haHandler.setIRLearner(&irLearner);
    
    Serial.println(F("系统初始化完成！"));
    
    // 初始化完成后关闭LED，进入待机状态
    ledControl.setState(LED_OFF);
    logger.log("system", "系统初始化完成");

    // 走到这里说明启动完整通过了：标记"这次启动是完整的"，下次启动不作为失败累加。
    // 不要提到更早的位置 —— 提前标记等于把"setup 中途卡住/崩掉"这条失败信号丢掉。
    SafeMode::setupCompleted();
}

void loop() {
    unsigned long now = millis();

    // ── 堆水位线（v1.3.31 埋点，只加日志）────────────────────────────────
    // 60 秒一行。掉的每一行都能回答一个此前答不了的问题：
    //   "设备是慢慢漏死的，还是掉 WiFi 之后才死的？"
    // 配合 wifi_manager 的 [wifi] 断开 / reconnect heap= 两条，构成完整时间线。
    // ⚠️ 60 秒一行、约 45 字节，同步忙等 ~4ms，对 1 秒 1 次的主循环可以忽略。
    {
        static unsigned long lastHeapLog = 0;
        if (lastHeapLog == 0) lastHeapLog = now;
        if (now - lastHeapLog >= 60000UL) {
            lastHeapLog = now;
            Serial.printf_P(PSTR("[heap] t=%lus heap=%u blk=%u frag=%u%%\n"),
                          now / 1000UL,
                          (unsigned)ESP.getFreeHeap(),
                          (unsigned)ESP.getMaxFreeBlockSize(),
                          (unsigned)(100UL - (ESP.getMaxFreeBlockSize() * 100UL) / (ESP.getFreeHeap() ? ESP.getFreeHeap() : 1)));
        }
    }

    // OTA 必须每次循环都轮询，否则 ArduinoOTA 收不到握手包。
    // 上传期间把所有 CPU 让给写 flash，避免 TCP 缓冲溢出导致刷写失败。
    otaService.handle();
    if (otaService.isUploading()) {
        yield();
        ESP.wdtFeed();
        return;
    }
    
    // 红外接收任务（最高优先级，10ms间隔）
    if (now - lastIRReceiverTime >= IR_RECEIVER_INTERVAL) {
        lastIRReceiverTime = now;
        
        // 学习模式下，跳过库解码，只使用自定义接收器
        if (!irLearner.isLearningDevice()) {
            // 发射期间 / 刚发射完的静默期内，接收头看到的是我们自己发出的光
            // —— IRsend 不关中断，GPIO5 在发射的几十毫秒里一直在采样。
            // 这段时间的采样必须丢掉：否则会自发自收回环（B 恰好是另一条规则的
            // A 时还会级联转发），而且发射结束后会解出一个半帧垃圾混进"最近信号"。
            if (irForwarder.isBusyTransmitting()) {
                irParser.resumeReceiver();
            } else {
                // 使用IRremoteESP8266库解码（方案A：直接使用库）
                UnifiedSignal signal;
                if (irParser.decodeWithLibrary(signal)) {
                    // 查找映射规则（用于转发）
                    IR_MapRule* rule = irMapper.findRule(signal);
                    // 检查信号是否在映射表中
                    bool matched = (rule != nullptr) || irMapper.isSignalInMappings(signal);
                    
                    // 记录所有信号（串口输出 + recentSignals缓冲区）
                    logger.logReceiveSignal(signal, matched);

                    // 告诉看门狗"刚有红外活动"：它会据此推迟网络探测（探测阻塞 ≤150ms，
                    // 会制造接收聋期），也把这一刻记进僵死快照的时间线。
                    NetWatchdog::noteIrActivity(now);
                    
                    // LED闪烁
                    ledControl.blink(1);
                    
                    // 查找映射并转发（forwardSignal内部会处理规则查找和重复码判断）
                    irForwarder.forwardSignal(signal);
                }
            }
        }
        
        // 保留自定义接收器用于学习模式（需要原始时序数据）
        irReceiver.update();
    }
    
    // 红外发射任务（高优先级，优化：减少到5ms间隔提升响应速度）
    if (now - lastIRTransmitterTime >= IR_TRANSMITTER_INTERVAL) {
        lastIRTransmitterTime = now;
        
        irForwarder.update();
        
        // LED 发射指示。
        // ⚠️ 以前判断的是 `irTransmitter.isTransmitting()`。但 irForwarder.update()
        // 里是**同步**把整条序列发完才返回的（含时序码的规则要几十~几百毫秒），
        // 等这一行执行到，transmitting 早就被置回 false 了 ⇒
        // `if (currentTransmittingState) ledControl.setState(LED_LONG_ON)` 这条分支
        // **永远不可达**：发射时 LED 从来不会亮，"LED_LONG_ON" 等于死代码。
        // 改成"距上次发射结束多久"：发射完成后保持亮一小会儿（肉眼可见），
        // 随后由 500ms 的网络任务把 LED 恢复成待机状态。
        bool currentTransmittingState = irTransmitter.isTransmitting() ||
                                        (irTransmitter.msSinceTxEnd() < TX_LED_HOLD_MS);
        
        if (currentTransmittingState) {
            ledControl.setState(LED_LONG_ON);
        }
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

        // WiFi 由「断线 → 连上」时，把当前配置记为已知良好（v1.3.9）
        // 之后主配置若被写坏或内容填错，可自动回退或用 AT+RESTORE 手动回退。
        static bool lastWifiConnected = false;
        if (wifiConnected && !lastWifiConnected) {
            networkConfig.markKnownGood();
        }
        lastWifiConnected = wifiConnected;

        ledControl.setNetworkStateWithMQTTConfig(wifiConnected, mqttConnected, mqttConfigured);

        // 堆见底兜底（v1.3.9）：先软恢复，连续见底 10 秒仍无起色就主动重启。
        // 主动重启好过等着崩 —— 崩了只剩 `Exception 29` + `rst cause:2`，现场不可控。
        HealthAction healthAction = HealthGuard::checkMemory();
        if (healthAction == HEALTH_SOFT_RECOVER) {
            if (forwardQueue.size() > 0) {
                forwardQueue.clear();   // 队列是定长数组，释放 0 字节，只丢待发任务
            }
            // ⚠️ 以前这里只调 logger.clearLogs()：它清的是 logs[10] 定长 char
            // 数组 ⇒ 释放 **0 字节**，末尾还要 saveToFlash() 分配 2 KB 写一次
            // flash ⇒ 在堆最紧张的时刻做了纯负收益的动作，"软恢复"因此形同虚设。
            // 真正占堆的是"最近信号序列"里那几段 Base64（堆分配），先释放它们。
            logger.clearRecentSequences();
            logger.clearLogs();
        } else if (healthAction == HEALTH_RESTART) {
            // ⚠️ 这条"留证据"的日志必须走 Serial 直写，不能走 logger.log()：
            // 进入 HEALTH_RESTART 的判据就是空闲堆 < 768 / 最大连续块 < 512，
            // 而 Logger::log() 第一行是 `freeHeap < 1024 && type != "error" ⇒ 丢弃`
            // ⇒ 用 logger.log("system", ...) 的话，这条日志在它**唯一该出现的
            // 时刻** 100% 被丢掉，needsSave 也不置位 ⇒ flush() 更是空转。
            // 于是事后只剩 RTC 里一个 lowMemRestarts 计数，说不清为什么重启。
            // 与上面"[面板] 内存见底"同理：这类最需要事后取证的事件必须直写串口。
            Serial.printf_P(PSTR("[内存] 堆见底，主动重启自愈 (空闲堆 %u B / 最大连续块 %u B)\n"),
                          (unsigned)ESP.getFreeHeap(),
                          (unsigned)ESP.getMaxFreeBlockSize());
            Serial.flush();
            ESP.restart();
        }

        // 稳定运行满 5 分钟 → 清零"连续因内存重启"计数（证明这一版撑得住）
        // ⚠️ 两处修正：
        //   ① `now >= HEALTH_STABLE_MS` 是本文件里**唯一**拿绝对阈值和时间比大小的
        //      定时判据（其余全用 `now - last >= 间隔`）。now 本身是 millis()，也就是
        //      "距开机多久"，所以这次比较碰巧是对的；但 millis() 一过 49.7 天回绕，
        //      它会重新变成 false，再等 5 分钟才恢复 —— 换成显式闩锁后这个隐患消失。
        //   ② 原来没有闩锁：理论上每 500ms 都会再判一次（noteStableRun 内部有
        //      _stableNoted 兜住，所以只是白判，不出错）。加闩锁，意图明确。
        static bool stableRunNoted = false;
        if (!stableRunNoted && now >= HEALTH_STABLE_MS) {
            stableRunNoted = true;
            HealthGuard::noteStableRun();
            NetWatchdog::noteStableRun();   // 同理：撑住了就清零"连续网络重启"计数
        }
        
        // Web服务器条件启动/停止逻辑（同时获取内存使用率，避免重复调用）
        float memUsagePercent = 0.0f;
        bool shouldRunWebServer = shouldWebServerRun(now, wifiConnected, mqttConnected, mqttConfigured, memUsagePercent);

        // 停止原因提示（区分"内存见底"与"WiFi 未连接"）
        if (!shouldRunWebServer && webServer.isRunning()) {
            if (MemoryMonitor::getFreeHeap() < MEMORY_WEBSERVER_STOP_HEAP ||
                MemoryMonitor::getLargestFreeBlock() < MEMORY_WEBSERVER_MIN_BLOCK) {
                DEBUG_PRINTF("[Web服务器] 内存见底，暂停面板 (空闲堆: %u B, 最大连续块: %u B)\n",
                             (unsigned)MemoryMonitor::getFreeHeap(),
                             (unsigned)MemoryMonitor::getLargestFreeBlock());
            }
        }
        
        // 根据条件启动/停止Web服务器
        if (shouldRunWebServer && !webServer.isRunning()) {
            webServer.start();
        } else if (!shouldRunWebServer && webServer.isRunning()) {
            webServer.stop();
        }
    }

    // 网络僵死自检（v1.3.36，NetWatchdog）—— 每 30 s 向网关做一次 TCP 探测，
    // 判据看**耗时**（正常 <20ms 返回；只有收发通路死了才耗满 150ms 超时）。
    // 连续 3 次耗满超时 ⇒ 写现场快照 + ESP.restart()，把"只能拔电"变成"自动重起"。
    // ⚠️ 位置必须在 OTA 判断之后（上面 isUploading 那条已经 return 过了），
    //    否则探测会阻塞 OTA 的收包。
    NetWatchdog::update(now);

    // Web服务器请求处理（每次主循环均处理，降低响应延迟）
    if (webServer.isRunning()) {
        webServer.handleClient();
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
    
    // 内存监控和自动清理（每30秒检查一次）
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
    
    // 非阻塞方式让出CPU，避免CPU占用过高
    // 使用yield()替代delay(1)，提升实时性
    yield();
}

