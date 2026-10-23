#ifndef CRASH_RECORD_H
#define CRASH_RECORD_H

#include <Arduino.h>

// ── 崩溃自留档（v1.3.50）────────────────────────────────────────────────────
//
// 【为什么要有它】
//   **212（正式机）没有串口**（只有开发机 111 接得到 USB-TTL，见 safe_mode.h:69
//   "封了壳之后没有串口"），所以串口那套 `>>>stack>>>` 崩溃现场对 212 **永远打不出来**。
//   2026-10-04 212 在空闲态自发崩了两次（`resetReason=2`），两次都只剩"崩过"这一条信息，
//   崩在哪、为什么崩，一点都不知道。
//
// 【做法】
//   Arduino ESP8266 核心在 `cores/esp8266/core_esp8266_postmortem.cpp` 里留了一个**弱符号**：
//       extern "C" void custom_crash_callback(rst_info*, uint32_t stack, uint32_t stack_end);
//   它在**崩溃当刻、栈还完好**时被调用（该文件 :276，紧挨着 `system_restart_local()`）。
//   我们覆盖它，把现场写进 **RTC 内存**（纯 RAM 写，崩溃现场唯一安全的地方），
//   重启后在开机流程里**正常地**落盘成 `/crash.json`，再由 `GET /api/diag/crash` 读出、
//   用 `.elf` 反解成函数名（`tools/decode_crash.py`）。
//
// ⛔ 改崩溃路径之前先读这段（踩了就是"为了诊断反而更崩"）：
//   1. 回调里**只碰 RAM**，绝不读写 flash、不落盘、不调 Serial —— 崩溃时中断已关，
//      任何需要中断的操作（flash 写、WiFi）都会直接挂死。
//   2. 记录必须**定长**（RTC 槽位是固定的 52 个字），不能按内容增长。
//   3. 记完不清 RTC：清的理由是"已经落盘成功"，落盘失败要留着下次开机再试。
//      ⇒ 清的动作只发生在 `collectOnBoot()` 里，且只在写盘成功之后。
//   4. RTC 槽位 word 12..63 是本模块专属，**别再分给别人**（见 safe_mode.h 顶部槽位总账）。
class CrashRecord {
public:
    // setup() 早期（文件系统就绪之后）调一次：把 RTC 里的现场搬到 flash。
    static void collectOnBoot();

    static bool   has();       // flash 里有没有现场
    static String readJson();  // 现场 JSON（没有则 "{}"）
    static void   clear();     // 删掉现场（不留 RTC，RTC 在落盘时就已失效）

    // RTC 用户区槽位：word 12..63 = 52 个字 = 208 B（user 区剩下来的全部）。
    static const uint32_t RTC_OFFSET   = 12;
    static const uint32_t RTC_WORDS    = 52;
    // 表头占 11 个字（magic / reason / exccause / epc1 / excvaddr / freeHeap /
    // uptimeMs / failAddr / failSize / stackStart / checksum），其余留给栈。
    static const uint32_t HEADER_WORDS = 11;
    static const uint32_t STACK_WORDS  = RTC_WORDS - HEADER_WORDS;   // 41

    // 供自测用：直接喂一份假的现场并写进 RTC（**不真的崩**）。
    // 参数就是核心回调的签名，见上面说明。
    static void captureForTest(uint32_t reason, uint32_t exccause,
                               uint32_t epc1, uint32_t excvaddr,
                               uint32_t failAddr, uint32_t failSize,
                               uint32_t stackStart, const uint32_t* stackWords,
                               uint32_t stackWordCount);
};

#endif  // CRASH_RECORD_H
