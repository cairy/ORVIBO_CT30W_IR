#include "net_watchdog.h"

#include "config/constants.h"
#include "utils/health_guard.h"
#include "utils/memory_monitor.h"
#include "utils/storage.h"

#include <ESP8266WiFi.h>

namespace {

const uint8_t HIST = 8;   // 快照里保留最近多少次探测

struct ProbeSlot {
    uint32_t at;         // 探测时刻（millis）
    uint16_t ms;         // 耗时
    uint8_t  outcome;    // 0=快(正常) 1=慢(耗满超时) 2=跳过
    uint8_t  connected;  // connect 是否返回成功（不参与判定，只记录）
};

ProbeSlot _hist[HIST];
uint8_t   _histIdx = 0;   // 下一个写入位置（缓冲满时它就是"最旧"的那条）
uint8_t   _histCnt = 0;

uint32_t _nextProbeAt = 0;   // 下一次允许探测的时刻
bool     _armed      = false;
uint32_t _armedAt    = 0;
uint32_t _lastOkAt   = 0;    // 最近一次探测"快"的时刻；0 = 从来没成功过
uint32_t _lastIrAt   = 0;    // 最近一次"**收到**"红外（只在接收路径更新）
uint32_t _lastIrSentAt = 0;  // 最近一次"**发射**"红外（v1.3.55；全部发射入口都走 sendSequence）
uint32_t _lastHttpAt = 0;
uint8_t  _failStreak = 0;    // 连续"慢"的次数
uint32_t _restartCount = 0;  // 连续因网络僵死重启的次数（RTC 持久化）
bool     _byNetwork  = false;// 本次启动是否由网络自愈触发

struct RtcData {
    uint32_t magic;
    uint32_t count;
    uint32_t flag;       // bit0 = 上次重启是网络自愈触发的
    uint32_t checksum;
};

}  // namespace

// ── RTC（跨重启）────────────────────────────────────────────────────────────
bool NetWatchdog::_rtcRead(uint32_t& count, uint32_t& flag) {
    RtcData d = {0, 0, 0, 0};
    if (!ESP.rtcUserMemoryRead(RTC_OFFSET, reinterpret_cast<uint32_t*>(&d), sizeof(d))) {
        return false;
    }
    if (d.magic != RTC_MAGIC) {
        return false;
    }
    if (d.checksum != (d.magic ^ d.count ^ d.flag ^ 0x5A5A5A5A)) {
        return false;
    }
    count = d.count;
    flag  = d.flag;
    return true;
}

bool NetWatchdog::_rtcWrite(uint32_t count, uint32_t flag) {
    RtcData d;
    d.magic    = RTC_MAGIC;
    d.count    = count;
    d.flag     = flag;
    d.checksum = d.magic ^ d.count ^ d.flag ^ 0x5A5A5A5A;
    return ESP.rtcUserMemoryWrite(RTC_OFFSET, reinterpret_cast<uint32_t*>(&d), sizeof(d));
}

// ── 生命周期 ────────────────────────────────────────────────────────────────
void NetWatchdog::begin() {
    uint32_t count = 0, flag = 0;
    if (_rtcRead(count, flag)) {
        _restartCount = count;
        _byNetwork = (flag & 1U) != 0;
        if (_byNetwork) {
            // 消费掉标志：保留"本次启动的原因"在本对象里，但把 RTC 置回 0，
            // 否则下一次普通重启也会被误当成"网络自愈重启"。
            _rtcWrite(_restartCount, 0);
        }
    }

    Serial.printf_P(PSTR("[看门狗] 网络僵死自愈%s：探测 %lus 一次 / 超时 %ums / 连续 %u 次判死 / 重启上限 %u\n"),
                  NET_WATCHDOG_ENABLED ? "已启用" : "已关闭",
                  NET_WD_INTERVAL_MS / 1000UL,
                  (unsigned)NET_WD_PROBE_TIMEOUT_MS,
                  (unsigned)NET_WD_FAIL_LIMIT,
                  (unsigned)NET_WD_MAX_RESTARTS);
    if (_restartCount > 0) {
        Serial.printf_P(PSTR("[看门狗] 连续网络重启计数: %lu\n"), (unsigned long)_restartCount);
    }
    if (_byNetwork) {
        Serial.println(F("[看门狗] 本次启动由网络僵死自愈触发，现场快照可在 /api/diag/freeze 取回"));
    }
}

void NetWatchdog::noteIrActivity(unsigned long now) {
    _lastIrAt = now;
}

void NetWatchdog::noteIrSent(unsigned long now) {
    _lastIrSentAt = now;
}

void NetWatchdog::noteHttpServed(unsigned long now) {
    _lastHttpAt = now;
}

void NetWatchdog::noteStableRun() {
    if (_restartCount == 0) {
        return;
    }
    Serial.printf_P(PSTR("[看门狗] 已稳定运行，清零连续网络重启计数（原 %lu）\n"),
                  (unsigned long)_restartCount);
    _restartCount = 0;
    _rtcWrite(0, 0);
}

// ── 探测 ────────────────────────────────────────────────────────────────────
NetWatchdog::ProbeOutcome NetWatchdog::_probe(uint16_t& elapsedMs, uint8_t& connected) {
    elapsedMs = 0;
    connected = 0;

    const IPAddress gw = WiFi.gatewayIP();
    if (gw[0] == 0 && gw[1] == 0 && gw[2] == 0 && gw[3] == 0) {
        return PROBE_SKIP;
    }

    WiFiClient c;
    c.setTimeout(NET_WD_PROBE_TIMEOUT_MS);
    const unsigned long t0 = millis();

    // 网关端口开着 → connect 成功（收到 SYN-ACK）；关着 → 收到 RST，connect 立即返回 0。
    // **两种都在 20 ms 内回家**，这正是整个判据的立足点：
    // 只有收发通路真的死了，才会一直等到超时。
    // ⚠️ 已知局限：若 tcp_new() 因内存耗尽立即失败，connect 会"快失败"⇒ 被判成正常。
    //    本模块针对的是"连 ARP 都不回"那种死法，不属此类；纯内存问题由 HealthGuard 兜。
    const int rc = c.connect(gw, NET_WD_PROBE_PORT);

    const unsigned long dt = millis() - t0;
    elapsedMs = (uint16_t)(dt > 65535UL ? 65535UL : dt);
    connected = rc ? 1 : 0;
    if (rc) {
        c.stop();
    }

    const uint16_t slowLine =
        (uint16_t)((uint32_t)NET_WD_PROBE_TIMEOUT_MS * NET_WD_SLOW_RATIO_NUM / NET_WD_SLOW_RATIO_DEN);
    return (elapsedMs >= slowLine) ? PROBE_SLOW : PROBE_OK;
}

void NetWatchdog::_record(uint32_t at, uint16_t ms, uint8_t outcome, uint8_t connected) {
    ProbeSlot& s = _hist[_histIdx];
    s.at        = at;
    s.ms        = ms;
    s.outcome   = outcome;
    s.connected = connected;
    _histIdx = (_histIdx + 1) % HIST;
    if (_histCnt < HIST) {
        _histCnt++;
    }
}

// ── 主循环钩子 ──────────────────────────────────────────────────────────────
NetWatchdog::Result NetWatchdog::update(unsigned long now) {
    if (!NET_WATCHDOG_ENABLED) {
        return RES_NONE;
    }

    if (!_armed) {
        _armed = true;
        _armedAt = now;
        _nextProbeAt = now;
        return RES_NONE;
    }

    // 启动保护期：刚上电时网络还在收敛（DHCP、首次 DNS），此时探测不通是正常的。
    if ((long)(now - _armedAt) < (long)NET_WD_ARM_AFTER_MS) {
        return RES_NONE;
    }

    // 前置：必须"自认已联网"。没连上时不通是**正常**状态（wifiManager 会去重连），
    // 不该由本模块重启 —— 否则会在路由器一断电的时候反复重启设备。
    if (WiFi.status() != WL_CONNECTED) {
        _failStreak = 0;
        return RES_NONE;
    }

    // 调试注入期间把间隔视为 0，让几次失败立刻连起来 —— 否则验证一次要等 3 × 30 s。
    const uint32_t interval = NET_WD_INTERVAL_MS;
    if ((long)(now - _nextProbeAt) < (long)interval) {
        return RES_NONE;
    }

    // 避让：刚收过**或刚发过**红外就先不探测 —— 探测会阻塞 ≤150 ms，制造一段接收聋期，
    // 连点场景下最容易正好打断一帧。这里**故意不推进 _nextProbeAt**，
    // 于是每一轮主循环都会重新检查，等安静下来立刻补上这次探测。
    // （调试注入期间跳过避让，否则现场有红外活动时注入不进去。）
    if ((_lastIrAt != 0 && (now - _lastIrAt) < NET_WD_QUIET_MS) ||
        (_lastIrSentAt != 0 && (now - _lastIrSentAt) < NET_WD_QUIET_MS)) {
        return RES_NONE;
    }

    uint16_t ms = 0;
    uint8_t  connected = 0;
    const ProbeOutcome outcome = _probe(ms, connected);
    _record(now, ms, (uint8_t)outcome, connected);
    _nextProbeAt = now;

    if (outcome == PROBE_SKIP) {
        return RES_NONE;
    }

    if (outcome == PROBE_OK) {
        if (_failStreak > 0) {
            Serial.printf_P(PSTR("[看门狗] 网络恢复（上次探测耗时 %u ms）\n"), (unsigned)ms);
        } else {
            // 成功也打一行：这是判断"探测到底有没有在跑"的唯一可见证据，
            // 同时给出局域网 RTT 基线（正常应远小于 150 ms 的超时）。
            // conn=1 表示网关 80 端口开着（收到 SYN-ACK），conn=0 表示端口关着但**回了 RST**
            // —— 两者都是"网络活着"，判据看的是耗时不是成败。
            Serial.printf_P(PSTR("[看门狗] 探测 ok %u ms conn=%u (heap %u)\n"),
                          (unsigned)ms, (unsigned)connected,
                          (unsigned)ESP.getFreeHeap());
        }
        _failStreak = 0;
        _lastOkAt = now;
        return RES_OK;
    }

    // ── 耗满超时：疑似僵死 ──────────────────────────────────────────────────
    _failStreak++;
    Serial.printf_P(PSTR("[看门狗] 探测耗满超时 %u ms（第 %u/%u 次），网关 %s\n"),
                  (unsigned)ms, (unsigned)_failStreak, (unsigned)NET_WD_FAIL_LIMIT,
                  WiFi.gatewayIP().toString().c_str());

    if (_failStreak < NET_WD_FAIL_LIMIT) {
        return RES_SLOW;
    }

    // 两道保险，任何一条不满足都只告警、不重启。
    // ① 从来没成功探测过 ⇒ 说明"网关根本不回包"（例如对未知端口静默丢弃），
    //    判据在当前网络不成立。不加这条，会在一个探测本来就不通的网络里反复误重启。
    if (_lastOkAt == 0) {
        Serial.println(F("[看门狗] 连续探测慢，但从未成功探测过：判据在当前网络不适用，不重启"));
        _failStreak = 0;
        return RES_SLOW;
    }
    // ② 连续网络重启已达上限 ⇒ 停止重启只告警，避免"重启 → 又僵 → 再重启"的死循环。
    if (_restartCount >= NET_WD_MAX_RESTARTS) {
        Serial.printf_P(PSTR("[看门狗] 已连续网络重启 %lu 次（上限 %u），停止重启、仅告警\n"),
                      (unsigned long)_restartCount, (unsigned)NET_WD_MAX_RESTARTS);
        _failStreak = 0;
        return RES_SLOW;
    }

    const uint32_t frozeFor = now - _lastOkAt;
    _failStreak = 0;
    _restartCount++;

    Serial.printf_P(PSTR("[看门狗] 判定网络僵死：连续 %u 次耗满超时，僵死约 %lu s。"
                         "写现场快照后重启自愈（第 %lu 次）\n"),
                    (unsigned)NET_WD_FAIL_LIMIT,
                    (unsigned long)(frozeFor / 1000UL),
                    (unsigned long)_restartCount);

    _writeSnapshot(now, frozeFor);
    // 先落快照再置 RTC：快照写失败也不能挡住"活过来"这件事。
    _rtcWrite(_restartCount, 1);
    Serial.flush();
    ESP.restart();
    return RES_RESTART;   // 不可达
}

// ── 现场快照 ────────────────────────────────────────────────────────────────
void NetWatchdog::_writeSnapshot(unsigned long now, uint32_t frozeForMs) {
    String s;
    s.reserve(NET_WD_SNAPSHOT_SIZE);

    s += F("{\"uptime\":");
    s += (unsigned long)(now / 1000UL);
    s += F(",\"resetReason\":");
    s += String((unsigned)HealthGuard::getResetReason());
    s += F(",\"resetReasonText\":\"");
    s += HealthGuard::getResetReasonText();
    s += F("\"");
    s += F(",\"wifiStatus\":");
    s += String((int)WiFi.status());
    s += F(",\"rssi\":");
    s += String(WiFi.RSSI());
    s += F(",\"ip\":\"");
    s += WiFi.localIP().toString();
    s += F("\",\"gateway\":\"");
    s += WiFi.gatewayIP().toString();
    s += F("\",\"ssid\":\"");
    s += WiFi.SSID();
    s += F("\"");
    s += F(",\"freeHeap\":");
    s += String((unsigned)MemoryMonitor::getFreeHeap());
    s += F(",\"maxFreeBlock\":");
    s += String((unsigned)MemoryMonitor::getLargestFreeBlock());
    s += F(",\"fragmentation\":");
    s += String((int)MemoryMonitor::getFragmentationPercent());
    s += F(",\"frozeForMs\":");
    s += frozeForMs;
    s += F(",\"lastIrAgoMs\":");
    s += (unsigned long)(_lastIrAt ? (now - _lastIrAt) : 0);
    // ⚠️ 与上一行**分开**：`lastIrAgoMs` = 最后**收到**，`lastIrSentAgoMs` = 最后**发射**。
    // 合起来看才能判"僵死是发射触发的、还是跟红外无关"（v1.3.55）。
    s += F(",\"lastIrSentAgoMs\":");
    s += (unsigned long)(_lastIrSentAt ? (now - _lastIrSentAt) : 0);
    s += F(",\"lastHttpAgoMs\":");
    s += (unsigned long)(_lastHttpAt ? (now - _lastHttpAt) : 0);
    s += F(",\"netRestarts\":");
    s += _restartCount;
    s += F(",\"probes\":[");
    for (uint8_t i = 0; i < _histCnt; i++) {
        const uint8_t idx = (_histCnt < HIST) ? i : (uint8_t)((_histIdx + i) % HIST);
        const ProbeSlot& p = _hist[idx];
        if (i) {
            s += F(",");
        }
        s += F("[");
        s += (unsigned long)(p.at / 1000UL);   // 探测时刻（uptime 秒）
        s += F(",");
        s += (unsigned)p.ms;
        s += F(",");
        s += (unsigned)p.outcome;
        s += F(",");
        s += (unsigned)p.connected;
        s += F("]");
    }
    s += F("]}");

    const bool ok = Storage::saveFileAtomic(CONFIG_FILE_FREEZE, s);
    Serial.printf_P(PSTR("[看门狗] 现场快照 %s（%u B）\n"),
                  ok ? "已保存" : "保存失败，仍将重启",
                  (unsigned)s.length());
}

// ── 查询 / 维护接口 ─────────────────────────────────────────────────────────
uint32_t NetWatchdog::getRestartCount() {
    return _restartCount;
}

bool NetWatchdog::didRestartByNetwork() {
    return _byNetwork;
}

bool NetWatchdog::hasSnapshot() {
    return Storage::fileExists(CONFIG_FILE_FREEZE);
}

String NetWatchdog::readSnapshot() {
    String s = Storage::readFile(CONFIG_FILE_FREEZE);
    if (s.length() == 0) {
        return String("{}");
    }
    return s;
}

void NetWatchdog::clearSnapshot() {
    Storage::deleteFile(CONFIG_FILE_FREEZE);
}

