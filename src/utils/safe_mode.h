#ifndef SAFE_MODE_H
#define SAFE_MODE_H

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include "network/network_config.h"
#include "utils/ota_service.h"

// 安全模式：只做串口日志 + 联网 + 固件上传，绕开一切可能出错的业务模块。
//
// 进入条件（双通道，互补）
//   1. 手动：上电瞬间按住 GPIO4 达 SAFE_MODE_CONFIRM_MS（零延迟设计：没按住就立刻放行）
//   2. 自动：RTC 内存中的启动失败计数 >= SAFE_MODE_MAX_FAILS
//
// ⭐ 自动计数的判据（v1.3.15 起）
//   一次启动算"失败"当且仅当**下面有一条成立**：
//     a. 上一次启动的复位原因是「硬件看门狗 / 异常(崩溃) / 软件看门狗」；或
//     b. 上一次启动没走到 setup() 末尾。
//   累加满 SAFE_MODE_MAX_FAILS 次即自动进安全模式。
//
//   为什么要加 (a)：旧写法只有 (b)（每次启动 +1、setup 末尾清零），
//   它**看不见"setup 走完了、一进业务循环就崩"的固件** —— 那种固件每次都能走到
//   setup 末尾把计数清掉，于是计数永远不涨、安全模式永远进不去，设备只能靠串口救。
//   而"新固件在业务代码里崩"恰恰是 OTA 之后最可能的坏法，所以必须按复位原因补上这一条。
//   （反过来说，(b) 也不能删：它覆盖"卡在 setup 中途且能复位"的情况。）
//
//   为什么 (a) 只认这 3 个原因、不认别的：`ESP.restart()`（含 OTA 完成后的重启）报
//   software-restart(4)、拔电报 power-on(0)、复位脚报 external-reset(6)，这三种都是
//   正常路径。实测证据：OTA 刷完的当次启动 `/api/status` 的 `resetReason` 就是 4
//   （2026-09-27 实测，且 bootFailCount 保持 0/3），所以反复刷机不会越刷越接近安全模式。
//
// ⚠️ 这套机制的前提是**设备会复位**。如果固件卡在一个带 delay()/yield() 的死循环里，
//   看门狗被喂饱、既不崩也不重启，计数就永远不涨、安全模式永远进不去。
//   ⇒ 业务代码里**禁止写 while(1)/for(;;) 死等**；确需重试就写"重试 N 次后 ESP.restart()"。
//   （v1.3.15 已清掉 main.cpp 里唯一的一处 LittleFS 死等。）
//   ⚠️ 这个盲区只剩"物理按钮"一条后路：上电时按住 GPIO4 手动进安全模式
//      （见下面的进入条件 1）—— 所以封壳时务必给这个按钮留出可按到的位置。
//
// 两条硬约束（踩了就是事故，别改）：
//   A. RTC 数据**只能**放在用户区 offset <= 63。offset 64..95 是 eboot 的 OTA 复制命令区
//      （Esp.cpp: rtcUserMemoryWrite -> system_rtc_mem(64+offset)；eboot_command 在 RTC_MEM=0x60001200）。
//
//      ⭐ 槽位总账（**这张表是权威**，另两个模块各写各的区间，改之前先看这里）：
//        word 0..3   SafeMode      magic / count / unfinished / checksum
//        word 4..6   HealthGuard   magic / count / checksum        （7 空着，留作对齐）
//        word 8..11  NetWatchdog   magic / count / flag / checksum （v1.3.36 起）
//      新模块从 12 之后接着排；用完 63 就没有了。三者的 magic 各不相同，
//      读错槽位只会因 magic 不匹配而被判"无效"，不会静默当成别的数据。
//   B. enter() 之后不再运行业务主循环与 buttonHandler.update()，否则 GPIO4 长按会走到
//      "清除配置"回调（main.cpp 的长按回调），把用户的 WiFi 配置清掉。
class SafeMode {
public:
    // 业务模式侧的计数维护，调用顺序见 main.cpp：noteBoot() 最早、setupCompleted() 在 setup() 末尾
    static void printBanner();       // 打印固件版本 + Sketch MD5，OTA 后用它确认刷没刷上去
    // 启动计数：lastBootCrashed = 上一次启动是不是崩溃/看门狗复位。
    // 返回**当前累计的可疑启动次数**（达 SAFE_MODE_MAX_FAILS 就该进安全模式）。
    static uint32_t noteBoot(bool lastBootCrashed);
    // 走到 setup() 末尾才调 —— 标记"这次启动是完整的"。
    // 注意不要提前调：提前调等于把"setup 中途卡住/崩掉"这条失败信号丢掉。
    static void setupCompleted();

    // 判定 + 进入（shouldEnter 内部含 GPIO 采样与 LED 提示）
    static bool shouldEnter();
    [[noreturn]] static void enter();  // 接管设备，不返回

    // 当前累计的"可疑启动"次数，供 /api/status 上报。
    // 用途：封了壳之后没有串口，只能靠这个字段远程确认"设备离安全模式还有多远"
    //      （0 就是干净的；到达 SAFE_MODE_MAX_FAILS 会自动进安全模式）。
    static uint32_t getBootFailCount();

    static bool isActive() { return _active; }

private:
    static bool _active;

    // RTC 用户区 offset 0..3（4 个字）—— 必须在 [0,63] 内，且不能让 HealthGuard(offset 4) 撞上
    static const uint32_t RTC_OFFSET = 0;
    // v1.3.15 布局加了 unfinished 字段，magic 跟着换 ⇒ 旧固件留在 RTC 里的计数自动作废（重建为 0）。
    // 升级到本版后第一次启动计数从 0 起算，是有意为之。
    static const uint32_t RTC_MAGIC = 0x5AFEC0DF;

    struct RtcBootData {
        uint32_t magic;
        uint32_t count;       // 连续"可疑启动"次数
        uint32_t unfinished;  // 上一次启动是否没走完 setup()：1 = 没走完
        uint32_t checksum;    // magic ^ count ^ unfinished ^ 0xA5A5A5A5
    };

    static bool _rtcRead(RtcBootData& data);
    static bool _rtcWrite(const RtcBootData& data);
    static void _clearFailCount();
    static uint32_t _readFailCount();

    static void _blinkConfirm();
    static void _serveRootPage(ESP8266WebServer& server);
};

#endif // SAFE_MODE_H
