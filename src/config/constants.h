#ifndef CONSTANTS_H
#define CONSTANTS_H

// ============================================================================
// 固件版本（OTA 后靠它 + Sketch MD5 确认到底刷没刷上去）
// ============================================================================
// 每次发布务必 +1；Sketch MD5 由 ESP.getSketchMD5() 运行时给出，开机串口会打印。
#define FW_VERSION "1.3.58"

// ============================================================================
// OTA 无线升级（ArduinoOTA 主通道 + 浏览器 /update 备用通道）
// ============================================================================
#define OTA_HOSTNAME "orvibo-ct30w"   // 网络主机名（同网段可直接用主机名刷）
#define OTA_PORT 8266                  // ArduinoOTA 端口（默认 8266）
#define OTA_USERNAME "admin"           // 浏览器 /update 页面的认证用户名

// OTA 口令**不再写在这里**：明文放 tracked 文件会随仓库（含历史）一起泄露，
// 已随本版本轮换。真值放在本机 secrets.ini（.gitignore 已排除），
// platformio.ini 在 [platformio] 段用 extra_configs 引入，并以
//     -D OTA_PASSWORD=\"${secrets.ota_password}\"
// 注入；模板见 secrets.example.ini，流程见 docs/flashing-and-ota.md §6。
// 两道编译期护栏：缺文件 / 空口令都直接编译失败，绝不会静默产出"无口令固件"。
#ifndef OTA_PASSWORD
#error "OTA_PASSWORD 未定义：本机缺 secrets.ini（照抄 secrets.example.ini 并填口令）"
#endif
static_assert(sizeof(OTA_PASSWORD) > 1, "OTA_PASSWORD 为空 —— 拒绝生成无口令固件");

// ---- HTTP API / 面板的访问口令（**可选**）----
// 留空 = 不启用鉴权（与 v1.3.18 及更早一致：压测脚本、长稳脚本、任何不带凭据的
// 自动化都还能跑）。
// 非空 = 面板首页与所有 /api/* 都要求 Basic 认证（用户名同 OTA_USERNAME）。
//   浏览器第一次打开面板会弹一次登录框，之后同源 fetch 会自动带上缓存的凭据
//   ⇒ 面板本身照常可用，但跨站页面和未授权的脚本被挡在外面。
// 由 platformio.ini 以 -D API_PASSWORD=\"${secrets.api_password}\" 注入。
#ifndef API_PASSWORD
#define API_PASSWORD ""
#endif

// ============================================================================
// 安全模式（上电按住 GPIO4 手动进 / 连续异常启动自动进）
// ============================================================================
#define SAFE_MODE_CONFIRM_MS 2000      // 上电按住 GPIO4 达到此时长才判定为手动进入
#define SAFE_MODE_MAX_FAILS 3          // RTC 连续"可疑启动"计数达到此值自动进入
#define SAFE_MODE_WIFI_WAIT_MS 15000   // 安全模式下等 WiFi 联网的最长时间
#define SAFE_MODE_IDLE_REBOOT_MS 600000 // 安全模式下无任何操作就自动重启回业务模式（10 分钟）

// ============================================================================
// 文件系统挂载失败的重试（v1.3.15）
// ----------------------------------------------------------------------------
// 挂载失败时的处置顺序：重试 N 次 → 格式化再挂载 → 仍失败则主动重启。
//
// ⚠️ 原实现是 `while(1) delay(1000)` 死等，那是个**空中无救**的坑：
//    delay() 喂饱看门狗 ⇒ 既不崩也不重启 ⇒ 安全模式的"可疑启动计数"永远不涨
//    ⇒ 设备永久不可用，只能拆壳接串口。整条可靠性策略的前提就是
//    "任何坏状态都能收敛到安全模式"，所以这里只允许"重试 + 重启"，不允许死等。
#define STORAGE_INIT_RETRY 3
#define STORAGE_INIT_RETRY_DELAY_MS 200

// ============================================================================
// 红外接收缓冲区
// ============================================================================
#define IR_RX_BUFFER_SIZE 2048       // 红外接收缓冲区大小（2KB，用于存储原始时序数据）

// IRremoteESP8266 库的采集缓冲项数（v1.3.24 从库默认的 100 提上来）
//
// 为什么必须提：实测风扇遥控器的帧 rawlen 一路涨到 99→100 并置 overflow=1，
// 而一条 NEC 帧只要 67 项 ⇒ 100 项（= 200µs）根本装不下，库永远只看到半个帧，
// 所有协议匹配必然失败（decode_type 落到 UNKNOWN），信号被整条丢弃 ——
// 表现就是"按了遥控器，最近信号里什么都没有"。
//
// 为什么改这里而不是改库：kRawBuf 是 IRrecv.h:25 的 const（不是宏，-D 覆盖不了），
// 但它是 IRrecv 构造函数的第 2 个参数默认值（IRrecv.h:124），
// 而 IRrecv.cpp:299 `params.rawbuf = new uint16_t[bufsize]` 真的按参数堆分配。
// ⇒ 显式传参即可，零库改动。详见 src/ir_core/ir_parser.cpp 的 init()。
//
// 400 项 = 800µs，覆盖库里最长的 A/C 协议还有余量。堆代价 (400-100)×2 = 600 B。
#define IR_CAPTURE_BUF_ITEMS 400

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

// 时序码"连发"时帧与帧之间的间隔（v1.3.26）
//
// ⚠️ 不能用 sendSequence 里那个默认的 100ms。原遥控器一次按键是**紧挨着**连发
//   7~12 遍同一帧，帧间隔实测 7,270µs（2026-10-03 学习模式原始波形）。
//   被控设备的接收端就是按这个节奏判重/去抖的；把间隔拉到 100ms（14 倍）
//   等于把一串"突发"拆成一条条孤立的帧 ⇒ 风扇不认（用户实测"发送无效"）。
//   实测值 7,270µs，取整到 7,270（不是 7ms —— µs 级延时用 delayMicroseconds，
//   PreciseTimer 的 ms 通道精度不够）。
#define TIMING_BURST_GAP_US 7270
#define REPEAT_CODE_INTERVAL_MIN_MS 100  // NEC重复码间隔下限（用于识别长按）
#define REPEAT_CODE_INTERVAL_MAX_MS 120  // NEC重复码间隔上限（NEC协议约108ms）
#define NEC_REPEAT_CODE 0xFFFFFFFF   // NEC重复码标识值（32位全1）

// 同一次按键的连发去重窗口（v1.3.24）
// 实测艾美特风扇遥控器一次按键连发 3~5 遍同一帧，遍与遍间隔 15~18ms
// （串口 [receive] 的 Interval 字段）。窗口取 200ms 覆盖得住，
// 又不会误伤"用户短时间内真的按了两次不同的键"
// ——那两次的 Base64 不同，本来就不靠这个判据区分。
#define TIMING_DEDUP_WINDOW_MS 200

// 帧间长间隔阈值：把"连发的多帧"切成"一帧"（v1.3.25）
//
// ⚠️ 为什么必须有：遥控器按一次会连发 3~12 遍同一帧，遍与遍之间只有 7.27ms
// 静默（远短于接收机判停的 15ms）⇒ 一次按键在接收缓冲里是**一整段**，
// 长度随"这一遍从哪条边沿起采"和"收到几遍"而变。
//
// 实测艾美特风扇遥控器（2026-10-03，学习模式取原始波形）：
//   帧内最长间隔 1,340µs / 帧间间隔 7,270~7,290µs / 一帧 23~24 点
//   ⇒ 采到的整段是 170 / 191 / 215 / 239 / 287 点，全都不一样。
// 后果有两条，都致命：
//   ① > MAX_LEARN_TIMING_COUNT(200) 的直接被丢弃（215/239/287 全没了）；
//   ② 侥幸 ≤200 的那些，每次长度不同 ⇒ Base64 不同 ⇒ 设为 A 之后再按
//      同一个键，signalsEqual 也认不出来 ⇒ **永远不转发**。
// ⇒ 必须切成"一帧"再存，判据是帧间那道长间隔。
//
// 阈值取 3,500µs：比帧内最大间隔(1,340)高一倍以上，比帧间间隔(7,270)低一倍以上。
// NEC 帧内最大间隔 1,690µs、Sony 1,200µs，都远在阈值之下，不会被误切。
// ⚠️ 只有库**认不出**的信号才会走到这里（认得出的走协议解码），所以不用担心
//    把某个已知协议从中间切开。
#define FRAME_GAP_MIN_US 3500

// 切出来的段至少要这么多点才算"一帧"；短于它的是采集起点处的残片
// （实测残片只有 1 点：那一段的第一条边沿是上一帧的收尾）。
// 与"短于 20 点当噪声"同档 —— 免得切出一个 8 点的段又被噪声闸拦回去。
#define MIN_TIMING_SEGMENT_POINTS 20

// 一次采集最多切出多少段（用于找"重复单元"）。
// 实测一按键连发 7~12 帧 ⇒ 段数 8~13。取 32 留足余量。
#define MAX_BURST_SEGMENTS 32

// 时序码比较容差（v1.3.25）
//
// ⚠️ 为什么不能用 Base64 精确匹配：实测同一按键连发 7 帧，**帧帧不同** ——
//   长标志 129→121µs 递减、短间隔 43→51µs 递增（占空比漂移，总周期不变）。
//   量化到 10µs 后就是 129→121、43→51，逐字节比不上。
//   ⇒ 精确匹配下 A/B 匹配与连发去重**永远不会命中**。
//
// 实测两类电平的分离度：短(350~520µs) 对 长(1210~1370µs)，间距 ≥690µs。
// 取 ABS 60µs / REL 30%：实测帧间同类最大差 80~90µs 稳稳通过，
// 跨类最小差 690µs 远大于容差 411µs 稳稳拦住（不会把两种电平混起来）。
// ⚠️ REL 不能取太小 —— 实测短电平本身在连发过程中就从 430µs 漂到 350µs（-19%），
//   20% 会卡在边界上（实测差 80µs vs 容差 86µs，只剩 6µs 余量）。
#define TIMING_MATCH_ABS_TOL_US 60
#define TIMING_MATCH_REL_PERCENT 30

// 红外热路径上的串口输出限流（v1.3.24）
//
// ⚠️ 为什么必须有：Serial.printf 在 ESP8266 上是**同步**的 ——
// UART FIFO 满之后 busy-wait 等发送完成。红外接收任务每 10ms 一轮
// （IR_RECEIVER_INTERVAL），而一条带 Base64 预览的日志约 130 字符
// = 115200 波特下 **11 ms** ⇒ 串口比帧间隔还慢，loop() 永远喂不到 WDT
// ⇒ 复位。实测 2026-10-02：一次按键打出 11214 行、設備直接重启
// （resetReason=4 software-restart，而堆是健康的、freeHeap 还有 10 KB）。
//
// ⇒ **这不是堆问题，是串口阻塞问题。** 两类死法要分开看：
//   ① 堆耗尽 → freeHeap 持续下降、Exception 29、resetReason=2
//   ② 串口阻塞 → 堆正常、resetReason=4 或 WDT，日志量巨大
// 判据：先看 freeHeap，再看串口行数。
//
// 取 1000ms：一次按键的 3~5 遍连发只打 1 行，既能看到"落时序码了"，
// 又不会拖垮主循环。
#define SERIAL_RATE_LIMIT_MS 1000

// ============================================================================
// 性能约束（确保实时性的关键参数）
// ============================================================================
#define FORWARD_DELAY_BEFORE_SEND_MS 20  // 转发前延时（AB信号隔离，防止干扰）
#define INTERRUPT_DISABLE_MAX_MS 10  // 发射时关闭中断最大时长（避免影响接收）

// 发射结束后的"接收静默期"。
//
// 它的**唯一**职责：让 10ms 一拍的接收任务（main.cpp 的 IR_RECEIVER_INTERVAL 分支）
// 至少跑一次 `irParser.resumeReceiver()`，把发射期间 ISR 采到的自家光从采集缓冲里
// 冲掉。20ms = 两次节拍，够用。防"自发自收回环/级联转发"的另一半职责已交给
// `IRTransmitter::isEchoOfLastTx()`（按码值判，见下）。
//
// ⚠️ 别再加长。聋期 = 20（AB 隔离）+ 发射占用 + 本值。而遥控器按住时是按固定
// 帧周期**重发整帧**的：只要聋期跨过"下一帧的第一个边沿"，那一帧就整条丢。
// 2026-10-01 实测（Xbox One 遥控器，规则 136 音量+，见 .workbuddy/memory/2026-10-01.md）：
//   · 该遥控器帧周期 T ≈ 253ms（真机按住音量+ 3 秒，记录间距 253/252/253/252/253/253/262ms）
//     ⇒ 下一帧的第一个边沿出现在"本次解码完成 +165ms"。
//   · 本值取 80 时聋期 208ms > 165ms ⇒ 每两帧吞一帧（按住音量时转速砍半）。
//   · 老固件（v1.2.0 之前）没有本值，聋期 128ms ⇒ 一帧不吞。
// 所以 80 → 20 不是"调参"，是把新版相对老固件的那段回归消掉。改后聋期约 109ms。
#define TX_SILENCE_AFTER_SEND_MS 20

// 自收回声的码值判据窗口：发射结束后这么久内解出的、码值等于刚发出去的那条 B 码，
// 判为自家回波（见 IRTransmitter::isEchoOfLastTx）。取 200ms：足够覆盖发射余晖与
// 接收头恢复，又短于任何遥控器的帧周期（通用遥控器 ≥108ms）⇒ 不会误伤真实按键，
// 因为真实按键的 A 码与被转发的 B 码本来就不是同一个值。
#define TX_ECHO_WINDOW_MS 200

// ── 通用 NEC 遥控器「按住不放」的续发窗口 ────────────────────────────────
// 通用遥控器按住时，第一帧发完整帧，之后每约 108ms 发一个**重复码 0xFFFFFFFF**
// （只表示"同一键还按着"，不含键值）。重复码按 A 码匹配不到任何规则，所以按住
// 只有第一帧会被转发 —— 表现为"按住音量+ 只加一格"。靠"最近命中的规则 + 本窗口"
// 把它续上（IRForwarder::forwardRepeat）。
// 取 500ms：遥控器重复周期约 108ms，窗口大于它就不会在按住中途断；也不能太长，
// 否则别的遥控器（或本机自己的回声）的重复码会被误当成"还在按上一键"。
#define TX_HOLD_REPEAT_WINDOW_MS 500
// 连续续发重复码的最小间隔。它挡的是"我们自己刚发出去的重复码被自己的接收头看到"
// —— 那种回声在发射后几毫秒就到，而遥控器的下一次重复在 ~108ms 后。取 60ms，两边都留余量。
#define TX_REPEAT_MIN_FORWARD_INTERVAL_MS 60

// 发射后 LED 保持点亮时长。main.cpp 的 10ms 任务里，irForwarder.update() 是**同步**
// 发完整条序列才返回的，等那一行执行到时 isTransmitting() 早就回到 false 了，于是
// "发射中亮灯"这条分支永远不成立（等于死代码）。改用"距上次发射结束多久"来点灯：
// 发射完成后继续亮这么久，肉眼能看见一次闪；必须**短于**网络任务的 500ms LED 刷新，
// 才能被及时收回待机状态。
#define TX_LED_HOLD_MS 150

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
// 单次连接尝试的等待窗口（非阻塞轮询，不忙等）。
// ⚠️ 2026-10-03 实机教训：原来只有**一个** `WIFI_CONNECT_TIMEOUT_MS = 5000` 被两处共用，
// 而 5 秒对"关联 + DHCP"来说恰好卡在边界上 —— 一旦真的断开，每轮都差一点、超时、
// 再等 5 秒，**设备永远回不来**（实测一次断开就把设备钉死在 AP 模式，只能串口 AT+RESTORE 救）。
// 开机首次连接从没暴露这个问题，因为 connect() 用的是 `while (attempts < 20) delay(500)` = 10 秒。
// 所以拆成两个，按"对旁人的影响"分别取值：
//   ① STA 模式重连：放宽到 15 秒 —— 不影响任何人，只是多等一会儿；
//   ② AP 模式回探：保持较短 —— 回探期间会切 AP+STA，正在连热点配网的手机要掉线。
#define WIFI_RECONNECT_TIMEOUT_MS 15000 // STA 模式：单次重连的等待窗口
#define WIFI_STA_PROBE_TIMEOUT_MS 8000  // AP 模式：单次 STA 回探的等待窗口（越短对配网手机越友好）
// PreciseTimer::delayPreciseMs 的等待宽限：超过 (ms + 这个值) 还没等到 Ticker 回调
// 就判定超时强制退出，避免"喂着看门狗死等"（那会让安全模式永远进不去）。
#define PRECISE_DELAY_GRACE_MS 50
#define AP_STA_PROBE_INTERVAL_MS 60000  // AP 模式下每隔多久回探一次原来的 WiFi（见 8.10）
// 周边 WiFi 扫描的兜底超时。扫描改成两段式起扫/取结果（见 web_server.cpp 的
// handleAPIWifiScan）：用户点了"搜索"然后直接关掉手机页面，就再也没人来取结果了，
// 不留兜底设备会一直停在"热点+站点"双模上。正常一次扫描 2~4 秒，20 秒足够宽裕。
#define WIFI_SCAN_TIMEOUT_MS 20000
#define AP_SSID "ORVIBO-CT30W"          // 配网模式AP热点名称（不是秘密，印在外壳上更好找）

// AP 口令**不再写在这里**：以前是明文 "12345678" 且进了 git（含历史）⇒
// 任何人都能连上这个热点，而热点一旦连上，设备上的改 WiFi、删规则、任意发红外
// 全是可直接调用的（见 web_server.cpp 的路由表）。
// 真值放在本机 secrets.ini，由 platformio.ini 以
//     -D AP_PASSWORD=\"${secrets.ap_password}\"
// 注入。缺文件/缺键直接编译失败，绝不静默退回一个公开口令。
#ifndef AP_PASSWORD
#error "AP_PASSWORD 未定义：请在 secrets.ini 里填 ap_password（照抄 secrets.example.ini）"
#endif
static_assert(sizeof(AP_PASSWORD) > 1, "AP_PASSWORD 为空 —— 拒绝生成无口令热点");

// ============================================================================
// MQTT 配置
// ============================================================================
// 主题定义
#define MQTT_TOPIC_COMMAND "homeassistant/orvibo-ct30w/command"  // 接收命令主题
#define MQTT_TOPIC_STATE "homeassistant/orvibo-ct30w/state"      // 状态上报主题
#define MQTT_TOPIC_LOG "homeassistant/orvibo-ct30w/log"          // 日志上报主题

// 缓冲区
// ⚠️ 这个定值**不依赖任何外部测量**，全部由"程序自己规定的最长信号"推出来：
//   ① 单信号最长 = MAX_LEARN_TIMING_COUNT(200) 点（本文件 :74）
//   ② 每点占 2 字节 ⇒ 400 B（ir_core/unified_signal.cpp:147 `byteCount = maxCount * 2`）
//   ③ 其文本形态 base64 = ceil(400/3)*4 = 536 字符（unified_signal.cpp:140 `timingToBase64`）
//   ④ 入站最长报文 = 3 B（固定头 + 长度域）+ topic + 536
//   ⑤ 出站最长报文 = Discovery「状态」常量模板 630 B（ha_interface/ha_mqtt_handler.cpp:74）
//      ⇒ 3 + topic + 630
// 取 topic 上限 64 B（`smartir/media_player/<name>/send` 量级），则
//   入站 3 + 64 + 536 = 603 B，出站 3 + 64 + 630 = 697 B
// ⇒ 收 1024 B：两侧都盖住，余量 ≥ 46%。
//
// 旧值 8192 是为 `query_maps` 留的 —— 它回完整映射表（满 20 条约 6.7 KB），
// 而该路径实测**已经发不出去**（`[MQTT: query_maps] Failed to publish message`），
// 等于这 8 KB 一直在为一条已死的能力占着堆，是"配了 MQTT 后空闲堆只剩 3.6 KB"的主因。
#define MQTT_BUFFER_SIZE 1024

// 单条入站 payload 的硬上限。取值依据同上 ①②③：最长合法信号 536 字符，
// 再留 JSON 包装（`{"action":"send_signal","code":"..."}` 约 34 B）与余量 ⇒ 768。
// 超过它的报文一律**显式拒绝并记账**，不让它一路走到解析器里被静默截断到 200 点 ——
// 静默截断的表现是"发出去的码只发了一半"，最难查。
#define MQTT_MAX_INBOUND_PAYLOAD 768

// Home Assistant Discovery 配置
#define MQTT_DISCOVERY_PREFIX "homeassistant"    // HA自动发现主题前缀
#define MQTT_DEVICE_ID "orvibo_ct30w"            // 设备唯一标识
#define MQTT_DEVICE_NAME "ORVIBO CT30W 红外转发器"  // 设备显示名称

// ============================================================================
// 存储文件路径（SPIFFS/LittleFS）
// ============================================================================
#define CONFIG_FILE_NETWORK "/network_config.json"  // WiFi/MQTT配置文件（主）
#define CONFIG_FILE_NETWORK_BAK "/network_config.bak"  // WiFi/MQTT配置备份（上一版，用于回退）
#define CONFIG_FILE_MAPS "/ir_maps.json"            // 映射规则配置文件
// 规则保存的临时文件：先写它，成功后 rename 覆盖 CONFIG_FILE_MAPS。
// 这样"写到一半失败/掉电"只会损失本次修改，不会把原规则文件清成半截 JSON。
#define CONFIG_FILE_MAPS_TMP "/ir_maps.tmp"
#define CONFIG_FILE_DEVICES "/devices.json"         // 设备/按钮配置（v1.3.10，从浏览器 localStorage 下沉到固件）
// 学习结果落盘（v1.3.46）：学完立刻写这里并释放 RAM —— 结果原本只在内存里，
// 而"学完之后"空闲堆会落到 ~4.5 KB，设备 3~4 分钟内会自行崩溃 ⇒ 结果必丢。
// 开始新的学习时删除（语义 = 开始学习即重置）。
#define CONFIG_FILE_LEARNED "/learned.json"
// 同一份结果的 json 变体（commands 的值是对象而不是字符串），只有 MQTT 的
// `query_learn_result` 带 format=json 时才用得到；面板一律用的是上面那份。
#define CONFIG_FILE_LEARNED_JSON "/learned_obj.json"
// ⭐ v1.3.47：落盘**条数**（内容就一个十进制数，几字节）。
//   存在的理由：`IRLearner::persistedCommandCount` 是 RAM 变量、重启即归零 ——
//   只留上面那份 JSON 的话，重启后 `getLearnedCommandCount()` 回 0，
//   /learn/result 就报 404「No learned device found」（2026-10-04 实测踩过）。
//   见 ir_learner.cpp 的 restorePersistedCount()。
#define CONFIG_FILE_LEARNED_COUNT "/learned.n"
// 设备配置的大小上限。
//
// ⚠️⚠️ 这个值的历史结论错过两次，两次都是"没量清楚就按内存估"：
//   ① 旧注释写"空闲堆约 12 KB ⇒ 3 KB 足够"，而空闲堆 2026-10 只剩约 8.2 KB；
//   ② 降到 2048 的理由是"框架收 body 读不满就 CLIENT_MUST_STOP"，方向对，但**上限**仍不该
//      由它决定 —— 见下。
//
// ⭐ v1.3.42 起上限不再受内存约束，理由是把链条两端的"整文件进 RAM"都拆掉了：
//   · 写：改成**分片提交**（`POST /api/devices/chunk`，见 DEVICES_CHUNK_MAX_BYTES）。
//     原来整份配置一次 POST，`ESP8266WebServer` 收 body 要一次读完（`Parsing-impl.h:160-169`），
//     读不满就丢连接 —— 实测学完 2 条时序码后（blk 只剩 4528）**1677 B 的配置必然收不下**，
//     而且失败形态是 TCP 被丢（浏览器只看到网络错误），连 4xx 都回不了。
//     ⛔ 加内存闸救不了：闸在 handler 里，而失败发生在更早的"收 body"阶段。
//   · 读：`GET /api/devices` 改成**流式发送**（chunked），不再把整文件读进 String
//     —— 原来峰值约 2×文件大小，8 KB 配置就会把空闲堆抽干。
//   ⇒ 现在上限只受 flash 空间与前端的展示能力约束。
//   ⚠️ 仍要留一个数：一是防滥用（前端/脚本写畸形大文件），二是分片路径的累计校验要用它。
#define MAX_DEVICES_FILE_SIZE 8192
#define LOG_FILE "/ir_logs.json"                    // 日志存储文件

// ── 设备配置的分片提交（v1.3.42）──────────────────────────────────────────
// 前端把配置按**字节**切成 ≤ 本值的片，逐片 POST /api/devices/chunk?seq=&total=&len=，
// 设备端追加写临时文件（devices.json.tmp），末片校验通过后 rename 成 devices.json。
//
// ⛔ 为什么是"字节"切片而不是"字符串"切片：设备端从不当单片是 JSON 解析，只做
//    字节追加；前端若按 JS 的 UTF-16 code unit 切，会把非 BMP 字符（emoji）的
//    surrogate pair 切开 ⇒ 单独的高代理经 UTF-8 编码变成 U+FFFD ⇒ 数据损坏。
//    所以前端先 TextEncoder 编成字节再切。
// ⛔ 为什么是 512：单片 body 在框架里峰值约 3×（plainBuf 一份、String 扩容时新旧块
//    短暂共存、`server.arg()` 又拷一份）⇒ 512 片只要 ~1.5 KB **连续**块，堆最紧
//    （学完 8 条指令）时也过得去；同时 8 KB 配置 = 16 片，串行发完约 2~4 秒。
#define DEVICES_CHUNK_MAX_BYTES 512

// 分片上传中的临时文件。命名与 `Storage::saveFileAtomic` 一致，便于识别与清理。
// ⭐ 半截上传不会永久占 flash：下一次 seq=0 会先删掉它（见 handleAPISaveDevicesChunk）。
#define DEVICES_TMP_FILE "/devices.json.tmp"

// `POST /api/devices`（一次性整份提交）的单发上限。
// ⛔ 这个值必须显著低于 `MAX_DEVICES_FILE_SIZE`：整份 POST 要框架**一次读完** body，
//    读不满就丢连接（连 4xx 都回不了）。实测可靠区上界约 1.8 KB（空闲堆 8.4 KB，且
//    是概率性的），取 1024 留足余量。超过它请走分片接口 —— 前端已这么做。
//    ⚠️ 别因为"MAX_DEVICES_FILE_SIZE 是 8192"就以为整份也能发 8 KB：那条路是给
//    分片接口用的，两者判的不是一回事。
#define DEVICES_SINGLE_SHOT_MAX 1024

// ============================================================================
// 内存管理阈值
// ============================================================================
#define MEMORY_MIN_FREE_HEAP 1024        // 最小空闲堆内存（字节）
#define MEMORY_MIN_FOR_JSON 2048         // JSON操作最小内存（字节）
#define MEMORY_MIN_FOR_MQTT 2048         // MQTT操作最小内存（字节）
#define MEMORY_MIN_FOR_WEBSERVER 4096    // Web服务器最小内存（字节）
#define MEMORY_MIN_FOR_FLASH_WRITE 4096  // Flash写入最小内存（字节）

// ── 「外部触发的发射入口」的总堆下限 ─────────────────────────────────────
// 为什么不能只靠 checkMemory(2048)：它第一道只要求 freeHeap ≥ 2048，
// 第二/三道看的是"使用率"和 maxBlock ⇒ 总堆只剩 3.6 KB 时**照样放行**。
// 2026-10-04 实机（v1.3.38，配了 MQTT）撞上一次：入口 heap=3680 被放行，
// 随后在内部分配失败 ⇒ SDK 打堆图（UMM_INFO，关中断、可到秒级）
// ⇒ Soft WDT 硬复位（resetReason=3）。
// ⛔ 别改成抬 maxBlock 门槛：实测 maxBlock=2328 的发送**能跑通**，
//    抬门槛会误杀本来正常的发送（用户看到的是"点发送没反应"）。
// 判据来自实测（556 个可信样本 = 串口 [TX] start × [API send] 配对）：
//   成功的发送，**请求入口** freeHeap 全部 ≥ 4536；崩溃那次是 3680
//   ⇒ 下界取两者之间的整数（v1.3.39 定为 4096）。
// ⛔⛔ **只用于请求入口**（payload 刚到、还没解析），不要挪到 IRForwarder::sendSequence()：
//   实测发射时的堆比请求入口**低约 1.4 KB**（min 3120 / p50 4504），
//   那次统计里 8 个 heap<4000 的发射**全部成功** ⇒ 挪过去会误杀入口 5000 左右的正常发送。
//   统一入口判据见 MemoryMonitor::hasSendHeadroom()（memory_monitor.h 有完整依据）。
// ⚠️ 代价：极低堆时发送会被拒（返回 500 Low Memory）。这是**有意的** —— 宁可偶发误拒，不要硬复位。
// v1.3.40 起覆盖面：POST /api/signals/send、MQTT send_signal、POST /api/forward/test、
//   HA/SmartIR 入站（smartir/*/send）。**仍未覆盖**：物理遥控器触发的转发（forwardSignal）。
// 发射路径要求的总空闲堆下限（字节）。
// ⭐ v1.3.44 起 4096 → 2560：**发射本身不再需要临时堆**。
//   sendSequence() 的时序码分支改用常驻的 IRForwarder::timingBuffer（原来在请求内部
//   new IRTiming[点数]，IRTiming = 8 B/点 ⇒ 168 点要 2368 B 连续堆）。4096 那个值就是
//   按「2368 + 请求期 String 开销」反推出来的。现在只需覆盖请求期基础开销
//   （payload / normalized / sequence 内各一份 Base64 ≈ 1.5 KB）+ 余量。
// ⚠️ 别再降到 2048 以下：发射期间每 10 ms yield() 一次会把 CPU 让给 WiFi/lwIP，
//   它们仍要几百字节。旧值 4096 的由来是「总堆 3.6 KB 放行 ⇒ 软看门狗硬复位」
//   （那一次发射要 2368 临时堆）；临时堆没了，余量可以放宽，但别取消。
#define MEMORY_MIN_FOR_SEND 2560

// ── 面板的内存兜底（只按绝对堆判断，不按"使用率"）────────────────────
// 为什么废弃百分比：使用率的分母是"启动早期空闲堆"的估算值（本机约 19.6KB），
// 面板正常水位就在 85% 上下，阈值 92% 离它只剩几个百分点 ⇒ 面板会"自己把自己关掉"，
// 表现就是面板时好时坏、ping 得通但 80 端口拒绝。
// 面板在 listen 空闲时几乎不占内存（真正吃内存的是处理请求的那一瞬间），
// 所以只有堆真的见底才让它避让，并留一条滞回带防止反复开关抖动。
#define MEMORY_WEBSERVER_STOP_HEAP    1024   // 空闲堆低于此值 → 停面板保命（与 MEMORY_MIN_FREE_HEAP 同级）
#define MEMORY_WEBSERVER_MIN_BLOCK    1024   // 最大连续块低于此值 → 停面板保命
#define MEMORY_WEBSERVER_RESUME_HEAP  2048   // 因内存停过后，空闲堆回升到此值才恢复

// ── 两条防"面板永久打不开"的护栏（2026-09-26 实测踩到，别再删）──────────
// 1) TRIP_CHECKS：只在"连续多次采样都低"时才判定危险。
//    处理一次 HTTP 请求的瞬间，空闲堆与最大连续块会短暂掉到阈值以下
//    （实测 maxBlock 968 / freeHeap 1808），单次采样就停面板
//    ⇒ 用户每打开一次页面就把面板打死一次。
// 2) RETRY_MS：停掉之后，即使内存一直没回升也强制重开一次。
//    旧版要求"最大连续块 ≥ 2048 才恢复"，而堆碎片率实测能到 46%，
//    该条件可能永远无法满足 ⇒ 面板被永久锁死，只能断电重启。
#define MEMORY_WEBSERVER_TRIP_CHECKS  6      // 连续 6 次（网络任务 500ms 一轮 ≈ 3 秒）
#define MEMORY_WEBSERVER_RETRY_MS     20000  // 停面板后最多 20 秒强制重开一次

// ── 堆见底的最后一道防线（v1.3.9，HealthGuard）────────────────────────────
// 上面那套是"停面板保命"；这里是"面板停了还继续掉"时的处置：
// 先软恢复（清队列 / 清日志），连续见底 10 秒仍无起色就**主动重启**。
// 为什么主动重启比等着崩好：崩溃是 `Exception 29` + `rst cause:2`，现场不可控，
// 而主动重启能在事前把原因写进日志、`/api/status` 也能报出 resetReason。
// 判据必须**低于**面板停止线（1024），否则会抢在"停面板"之前乱重启。
#define MEMORY_PANIC_HEAP          768    // 空闲堆低于此值 → 进入见底流程
#define MEMORY_PANIC_BLOCK         512    // 最大连续块低于此值 → 进入见底流程
#define MEMORY_PANIC_TRIP_CHECKS   20     // 连续 20 次（网络任务 500ms 一轮 = 10 秒）
#define MEMORY_PANIC_MAX_RESTARTS  3      // 连续因内存重启达此次数 → 停止重启，只保命
                                          // （防"重启 → 又见底 → 再重启"的死循环）
#define HEALTH_STABLE_MS           300000 // 稳定运行 5 分钟后，清零连续内存重启计数

// ── 网络僵死自愈（v1.3.36，NetWatchdog）────────────────────────────────────
// 要治的不是"堆见底"，而是另一种死法：设备**主循环活着、WiFi 自认连着、
// 但 LAN 上彻底消失**（ping 不成、ARP 表里没有它、TCP/UDP 全无应答），
// 不崩、不复位、3 分钟也不自愈，只能断电。实测此时堆还剩 4~6 KB 连续块，
// 且无 OOM / 无 Exception / 无 WDT ⇒ 与"内存耗尽"的签名正好相反。
//
// 判据用"出站探测的**耗时**"，不用"有没有收到请求"：
//   局域网里无论网关端口开着（回 SYN-ACK）还是关着（回 RST），都在 20 ms 内返回；
//   **只有收发通路真的死了才会一直等到超时**。所以"耗满超时"才是异常信号。
//   （用"多久没收到请求"必错：用户关掉面板后本来就没有请求。）
//
// 代价：探测是阻塞的，会制造一段接收聋期。故三重约束：
//   ① 超时压到 150 ms —— 聋期判据是"聋期 < 帧周期 − 85 ms"，
//      Xbox 类遥控器帧周期约 250 ms ⇒ 安全线约 165 ms，150 ms 在线内；
//   ② 间隔 30 s，撞上按键的概率约 0.5%；
//   ③ 距上次收到红外不足 500 ms 时**不探测**，顺延到下一轮。
#define NET_WATCHDOG_ENABLED      1        // 置 0 = 整套守卫编译掉，回到旧行为（对照实验用）
#define NET_WD_INTERVAL_MS        30000UL  // 探测间隔
#define NET_WD_PROBE_TIMEOUT_MS   150      // 单次探测超时（ms）
#define NET_WD_PROBE_PORT         80       // 探测目标端口（网关 80：开→SYN-ACK，关→RST，两者都快）
#define NET_WD_SLOW_RATIO_NUM     3        // 耗时 >= 超时 × 3/4 即判为"慢"（正常应 < 20 ms）
#define NET_WD_SLOW_RATIO_DEN     4
#define NET_WD_FAIL_LIMIT         3        // 连续"慢"几次 ⇒ 判定僵死（3 × 30 s ≈ 90 s）
#define NET_WD_MAX_RESTARTS       3        // 连续因网络僵死重启达此次数 ⇒ 只告警、不再重启
#define NET_WD_ARM_AFTER_MS       60000UL  // 启动满 60 s 才开始判定（避开开机阶段的网络抖动）
#define NET_WD_QUIET_MS           500UL    // 距上次红外活动至少这么久才探测
#define NET_WD_SNAPSHOT_SIZE      640      // 快照字符串预留（约 400 B 实际内容）
#define CONFIG_FILE_FREEZE        "/freeze.json"  // 僵死现场快照（覆盖式，只留最近一次）

// 崩溃自留档（v1.3.50）：212 没有串口 ⇒ 崩溃栈只能靠固件自己留。
//   custom_crash_callback 写 RTC（word 12..63，见 safe_mode.h 的槽位总账），
//   开机时 CrashRecord::collectOnBoot() 落成这个文件，GET /api/diag/crash 读出。
#define CONFIG_FILE_CRASH         "/crash.json"

// ============================================================================
// 调试宏（取消下行注释以启用串口调试输出）
// ============================================================================
// #define DEBUG_ENABLED

// 崩溃自留档自测（v1.3.50）：置 1 后，开机时若还没有现场就伪造一份崩溃现场并重启
// （**不真的崩**，一发即止），用来验证 RTC → 落盘 → /api/diag/crash → decode_crash.py
// 整条链路。验证完记得关掉。
// ⚠️ 用环境变量开它（`PLATFORMIO_BUILD_FLAGS="-DCRASH_TEST=1" pio run`）会**全量重编**，
//    大约要几分钟；直接改这一行只重编 src/，几十秒。二选一。
// #define CRASH_TEST 1

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
