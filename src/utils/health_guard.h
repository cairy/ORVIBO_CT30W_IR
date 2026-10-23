#ifndef HEALTH_GUARD_H
#define HEALTH_GUARD_H

#include <Arduino.h>

// 运行时健康守卫（v1.3.9）
//
// 只做两件实事：
//   1. 记住**上次重启原因**（`system_get_rst_info()` 的 reason），经 `/api/status` 上报。
//      用途：设备重启后一眼分清是"被压崩了"还是"自己主动重启 / OTA 重启"，
//      以前只能事后从串口猜。
//   2. 堆见底时先软恢复（停面板 / 清队列 / 清日志），持续无效则**主动干净重启**——
//      主动重启比等着崩溃（`Exception 29` + `rst cause:2`）干净，也更可控。
//      并用 RTC memory 记"连续因内存重启的次数"，到上限就停止重启只保命，
//      避免"重启 → 又见底 → 再重启"的死循环。
//
// ❌ 不做喂狗定时器：ESP8266 core 默认已开软硬件 WDT，`loop()` 末尾还有 `ESP.wdtFeed()`，
//    主循环卡死由 WDT 兜住；而"重启循环保护"已由 `SafeMode` 实现
//    （RTC 记启动失败计数，连续 `SAFE_MODE_MAX_FAILS` 次自动进安全模式）。
//    再叠加一个应用层喂狗只会增加误重启的风险，不做。
// 堆见底时的处置动作（由 main.cpp 执行，守卫本身只判定、不碰业务对象）
enum HealthAction {
    HEALTH_OK = 0,            // 堆正常
    HEALTH_SOFT_RECOVER = 1,  // 刚见底：调用方去清队列 / 清日志，看能不能救回来
    HEALTH_RESTART = 2        // 软恢复后仍见底：调用方记一笔日志后主动重启
};

class HealthGuard {
public:
    // setup() 里尽早调用（在文件系统初始化之前也行，不依赖 LittleFS）
    static void begin();

    static uint8_t getResetReason();
    static const char* getResetReasonText();

    // 上一次重启是不是"非正常"的（硬件看门狗 / 异常崩溃 / 软件看门狗）。
    //
    // 用途：`SafeMode::noteBoot()` 拿它决定要不要累加"可疑启动"计数。
    // 刻意**不包含** software-restart(4) / power-on(0) / external-reset(6)：
    //   OTA 完成后的重启、主动 `ESP.restart()`、拔电都属正常路径，
    //   把它们算成失败会让"连着刷两次固件"直接误进安全模式。
    static bool isAbnormalReset();

    // 每次网络任务（500ms 一轮）调用一次。
    // 触发顺序固定为「先 SoftRecover 一次 → 连续见底达阈值才 Restart」，
    // 且 Restart 受连续重启次数上限保护（防死循环）。
    static HealthAction checkMemory();

    // 设备稳定运行一段时间后调用（main.cpp 里是 uptime > 5 min 时调一次），
    // 用于清零"连续内存重启"计数——证明这一版真能撑住。
    static void noteStableRun();

    static uint32_t getLowMemRestartCount();

private:
    static uint8_t _resetReason;
    static uint32_t _lowMemRestarts;   // 连续因内存见底重启的次数（来自 RTC）
    static uint16_t _tripCount;        // 当前这一轮连续见底的采样数
    static bool _stableNoted;
    static bool _softRecovered;        // 本轮是否已做过软恢复

    // RTC 用户区偏移：SafeMode 占 0..3，这里占 4..6，NetWatchdog 占 8..11。
    // ⚠️ 硬约束（同 SafeMode）：offset 必须 <= 63，64..95 是 eboot 的 OTA 复制命令区。
    //   槽位总账见 safe_mode.h 顶部（那边是权威）。
    static const uint32_t RTC_OFFSET = 4;
    static const uint32_t RTC_MAGIC = 0xEA17C0DE;

    struct RtcData {
        uint32_t magic;
        uint32_t count;
        uint32_t checksum;   // magic ^ count ^ 0x5A5A5A5A
    };

    static bool _rtcRead(RtcData& data);
    static bool _rtcWrite(const RtcData& data);
};

#endif // HEALTH_GUARD_H
