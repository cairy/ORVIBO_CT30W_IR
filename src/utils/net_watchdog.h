#ifndef NET_WATCHDOG_H
#define NET_WATCHDOG_H

#include <Arduino.h>

// 网络僵死自愈守卫（v1.3.36）
//
// 治的是 HealthGuard 管不到的那种死法：**主循环活着、WiFi 自认连着、
// 但 LAN 上彻底消失**（ping 不成、路由器 ARP 表里没有它、TCP/UDP 全无应答），
// 不崩、不复位、不自愈，只能断电。实测此时堆还剩 4~6 KB 连续块，
// 无 OOM / 无 Exception / 无 WDT ⇒ 与"内存耗尽"签名正好相反，所以不是 HealthGuard 的活。
//
// 判据（全在 constants.h 有注释）：每 NET_WD_INTERVAL_MS 向网关做一次 TCP 探测，
// **看耗时**而不是看成败 —— 局域网上无论端口开(回 SYN-ACK)还是关(回 RST)都在 20 ms 内返回，
// 只有收发通路真死了才会一直等到超时。连续 NET_WD_FAIL_LIMIT 次耗满超时 ⇒ 判僵死。
//
// 判定僵死后：先写现场快照到 flash（重启后可从 /api/diag/freeze 取回），再 ESP.restart()。
// 这样"死一次要人工拔电 + 现场只在串口"变成"死一次自动重起 + 现场自动存档"。
//
// ⚠️ 与 HealthGuard 的分工（别合并）：HealthGuard 管**堆见底**并软恢复；
// 本模块管**网络不通**且只有重启一条路。两者 RTC 槽位分开（SafeMode 0~2、
// HealthGuard 4~6、本模块 8~11），各自的"连续重启计数"互不干扰。
class NetWatchdog {
public:
    enum Result {
        RES_NONE = 0,     // 未到时机 / 前置条件不满足（未联网、未过启动保护期、刚收过红外）
        RES_OK   = 1,     // 探测快速返回 ⇒ 网络收发通路活着
        RES_SLOW = 2,     // 探测耗满超时 ⇒ 疑似僵死（失败计数 +1，未达阈值）
        RES_RESTART = 3   // 已判定僵死、已落快照、即将重启（本返回值之后不会再回到主循环）
    };

    // setup() 中调用（放在 HealthGuard::begin() 附近，不依赖 LittleFS 读写）
    static void begin();

    // 主循环调用。内部完成"到点探测 → 判定 →（判僵死时）落快照 + 重启"。
    static Result update(unsigned long now);

    // 收到红外 / 成功处理一次 HTTP 请求时调用：
    //   ① 探测避让 —— 刚收过红外就不探测，绝不制造聋期打断连点；
    //   ② 记进快照，重启后能看出"僵死是从什么时候开始的"。
    // ⚠️ 本函数只代表**收到**。发射是另一条路径（`IRForwarder::sendSequence()`），
    //    走 noteIrSent()，两个时刻在快照里是**分开**的两个字段 —— 别合并，
    //    否则"是收还是发触发的"就分不出来了（v1.3.55 的由来）。
    static void noteIrActivity(unsigned long now);

    // 发射红外时调用（`IRForwarder::sendSequence()` 开头那一处，覆盖全部发射入口）。
    // 作用同上：避让探测 + 记进快照的 `lastIrSentAgoMs`。只写一个 uint32，不开堆。
    static void noteIrSent(unsigned long now);
    static void noteHttpServed(unsigned long now);

    // 稳定运行满一段时间后调用（与 HealthGuard::noteStableRun 同一处），
    // 证明这版撑住了 ⇒ 清零"连续因网络僵死重启"计数，避免累计到上限后不再自愈。
    static void noteStableRun();

    static uint32_t getRestartCount();     // 连续因网络僵死重启的次数（来自 RTC）
    static bool didRestartByNetwork();     // 本次启动是不是网络自愈触发的
    static bool hasSnapshot();             // flash 里有没有上次僵死的现场
    static String readSnapshot();          // 读现场（没有则返回 "{}"）
    static void clearSnapshot();           // 删现场

private:
    enum ProbeOutcome { PROBE_OK = 0, PROBE_SLOW = 1, PROBE_SKIP = 2 };

    // 探测一次。connected 只是"connect 有没有返回成功"（网关端口开着=1、关着=0），
    // **不参与判定**，只写进快照供事后分析（比如区分"慢+连上"与"慢+被拒"）。
    static ProbeOutcome _probe(uint16_t& elapsedMs, uint8_t& connected);
    static void _record(uint32_t at, uint16_t ms, uint8_t outcome, uint8_t connected);
    static void _writeSnapshot(unsigned long now, uint32_t frozeForMs);
    static bool _rtcRead(uint32_t& count, uint32_t& flag);
    static bool _rtcWrite(uint32_t count, uint32_t flag);

    // RTC 用户区：SafeMode 占 0~3，HealthGuard 占 4~6，本模块占 8~11。
    // ⚠️ 硬约束（同 SafeMode）：offset 必须 <= 63，64~95 是 eboot 的 OTA 复制命令区。
    //   槽位总账见 safe_mode.h 顶部（那边是权威）。
    static const uint32_t RTC_OFFSET = 8;
    static const uint32_t RTC_MAGIC  = 0xB17C0DE5;
};

#endif // NET_WATCHDOG_H
