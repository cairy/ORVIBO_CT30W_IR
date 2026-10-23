#include "health_guard.h"
#include "config/constants.h"
#include "utils/memory_monitor.h"
#include <user_interface.h>   // system_get_rst_info()

// REASON_* 枚举（tools/sdk/include/user_interface.h:58-64），这里抄一份注释备查：
//   0 上电   1 硬件看门狗   2 异常复位（崩溃）   3 软件看门狗
//   4 软件重启（OTA / ESP.restart）   5 深睡唤醒   6 外部复位
static const char* const RESET_REASON_TEXT[] = {
    "power-on",             // 0
    "hardware-watchdog",    // 1
    "exception",            // 2  ← 崩溃（含 OOM 后的 Exception 29）
    "software-watchdog",    // 3
    "software-restart",     // 4  ← OTA 完成 / 主动 ESP.restart()
    "deep-sleep-awake",     // 5
    "external-reset"        // 6
};

uint8_t HealthGuard::_resetReason = 0;
uint32_t HealthGuard::_lowMemRestarts = 0;
uint16_t HealthGuard::_tripCount = 0;
bool HealthGuard::_stableNoted = false;
bool HealthGuard::_softRecovered = false;

bool HealthGuard::_rtcRead(RtcData& data) {
    data.magic = 0;
    data.count = 0;
    data.checksum = 0;
    if (!ESP.rtcUserMemoryRead(RTC_OFFSET, reinterpret_cast<uint32_t*>(&data), sizeof(data))) {
        return false;
    }
    if (data.magic != RTC_MAGIC) {
        return false;
    }
    if (data.checksum != (data.magic ^ data.count ^ 0x5A5A5A5A)) {
        return false;
    }
    return true;
}

bool HealthGuard::_rtcWrite(const RtcData& data) {
    RtcData out = data;
    out.magic = RTC_MAGIC;
    out.checksum = out.magic ^ out.count ^ 0x5A5A5A5A;
    return ESP.rtcUserMemoryWrite(RTC_OFFSET, reinterpret_cast<uint32_t*>(&out), sizeof(out));
}

void HealthGuard::begin() {
    rst_info* info = system_get_rst_info();
    _resetReason = info ? info->reason : 0;

    RtcData data;
    _lowMemRestarts = _rtcRead(data) ? data.count : 0;

    Serial.printf_P(PSTR("[健康] 上次重启原因: %u (%s)"), _resetReason, getResetReasonText());
    if (_lowMemRestarts > 0) {
        Serial.printf_P(PSTR(" | 连续内存重启: %u"), (unsigned)_lowMemRestarts);
    }
    Serial.println();
}

uint8_t HealthGuard::getResetReason() {
    return _resetReason;
}

const char* HealthGuard::getResetReasonText() {
    if (_resetReason < sizeof(RESET_REASON_TEXT) / sizeof(RESET_REASON_TEXT[0])) {
        return RESET_REASON_TEXT[_resetReason];
    }
    return "unknown";
}

bool HealthGuard::isAbnormalReset() {
    // 1 硬件看门狗 / 2 异常复位(崩溃) / 3 软件看门狗 —— 这三种是"固件/内存真出问题了"的信号
    return _resetReason == 1 || _resetReason == 2 || _resetReason == 3;
}

uint32_t HealthGuard::getLowMemRestartCount() {
    return _lowMemRestarts;
}

HealthAction HealthGuard::checkMemory() {
    // 与 /api/status 同口径，避免两套读数互相打架
    const uint32_t freeHeap = MemoryMonitor::getFreeHeap();
    const uint32_t maxBlock = MemoryMonitor::getLargestFreeBlock();

    // 见底判据：总量不够，或者总量够但拿不出一块连续内存（后者才是真正的崩溃前兆）
    const bool starved = (freeHeap < MEMORY_PANIC_HEAP) || (maxBlock < MEMORY_PANIC_BLOCK);

    if (!starved) {
        if (_tripCount > 0) {
            Serial.printf_P(PSTR("[健康] 堆已恢复 (空闲堆 %u B / 最大连续块 %u B)\n"),
                          (unsigned)freeHeap, (unsigned)maxBlock);
        }
        _tripCount = 0;
        _softRecovered = false;
        return HEALTH_OK;
    }

    _tripCount++;

    // 先给软恢复一次机会（由 main.cpp 执行清队列 / 清日志）
    if (!_softRecovered) {
        _softRecovered = true;
        Serial.printf_P(PSTR("[健康] 堆见底，先尝试软恢复 (空闲堆 %u B / 最大连续块 %u B)\n"),
                      (unsigned)freeHeap, (unsigned)maxBlock);
        return HEALTH_SOFT_RECOVER;
    }

    if (_tripCount < MEMORY_PANIC_TRIP_CHECKS) {
        return HEALTH_OK;
    }

    // 连续见底已达阈值 —— 但连续内存重启到上限就不再重启，只保命并告警，
    // 否则会陷入"重启 → 又见底 → 再重启"的死循环。
    if (_lowMemRestarts >= MEMORY_PANIC_MAX_RESTARTS) {
        Serial.printf_P(PSTR("[健康] 堆持续见底但已连续内存重启 %u 次，停止重启、仅保命 "
                             "(空闲堆 %u B / 最大连续块 %u B)\n"),
                        (unsigned)_lowMemRestarts, (unsigned)freeHeap, (unsigned)maxBlock);
        _tripCount = 0;   // 不清零会每轮狂打日志；清零后每 MEMORY_PANIC_TRIP_CHECKS 轮报一次
        _softRecovered = true;   // 别再走软恢复分支，否则每轮都会返回 SOFT_RECOVER
        return HEALTH_OK;
    }

    Serial.printf_P(PSTR("[健康] 堆持续见底 %u 次采样，主动重启以自愈 "
                         "(空闲堆 %u B / 最大连续块 %u B，连续内存重启计为 %u)\n"),
                    (unsigned)_tripCount, (unsigned)freeHeap, (unsigned)maxBlock,
                    (unsigned)(_lowMemRestarts + 1));

    // 先把计数写进 RTC，重启后才知道"这是第几次因为内存而重启"
    RtcData data;
    data.count = _lowMemRestarts + 1;
    if (!_rtcWrite(data)) {
        Serial.println(F("[健康] 警告: RTC 写入失败，内存重启计数无法持久化"));
    }

    return HEALTH_RESTART;   // 由 main.cpp 记日志 + ESP.restart()
}

void HealthGuard::noteStableRun() {
    if (_stableNoted || _lowMemRestarts == 0) {
        return;
    }
    _stableNoted = true;
    Serial.println(F("[健康] 已稳定运行，清零连续内存重启计数"));
    RtcData data;
    data.count = 0;
    _rtcWrite(data);
    _lowMemRestarts = 0;
}
