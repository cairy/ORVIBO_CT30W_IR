#ifndef MEMORY_MONITOR_H
#define MEMORY_MONITOR_H

#include <Arduino.h>

// 内存监控工具类
class MemoryMonitor {
public:
    // 初始化内存监控（在启动时调用，记录初始堆大小）
    // earlyFreeHeap: setup() 最开始记录的空闲堆（可选，0表示自动检测）
    static void init(size_t earlyFreeHeap = 0);
    
    // 获取空闲堆内存（字节）
    static size_t getFreeHeap();
    
    // 获取已使用堆内存（字节）
    static size_t getUsedHeap();
    
    // 获取最大连续空闲块大小（字节）
    static size_t getLargestFreeBlock();
    
    // 获取最小空闲块大小（字节）
    // 注意：ESP8266 不支持 getMinFreeHeap，返回当前空闲堆作为近似值
    static size_t getMinFreeHeap();
    
    // 获取内存使用百分比（0-100）
    static float getUsagePercent();
    
    // 获取内存碎片率（0-100，值越大碎片越多）
    // 碎片率 = (总空闲内存 - 最大连续块) / 总空闲内存 * 100
    static float getFragmentationPercent();
    
    // 检查内存使用是否超过阈值（默认50%）
    static bool isMemoryUsageHigh(float thresholdPercent = 50.0f);
    
    // 打印内存使用信息到串口
    static void printMemoryInfo();
    
    // 获取内存使用信息字符串
    static String getMemoryInfoString();
    
    // 统一的内存检查函数
    // requiredBytes: 所需的最小连续内存块大小（字节）
    // maxUsagePercent: 最大允许的内存使用率（默认93%）
    // 返回true表示内存充足，false表示内存不足
    static bool checkMemory(size_t requiredBytes, float maxUsagePercent = 93.0f);

    // ── 发射路径专用的「总堆下限」（v1.3.39 起）────────────────────────────
    // 为什么 checkMemory() 不够：它第一道只要求 freeHeap ≥ requiredBytes，
    // 第二/三道看「使用率」与「最大连续块」⇒ 总堆只剩 3.6 KB 时**照样放行**。
    // 2026-10-04 实测那次放行的后果不是"发送失败"，是**软看门狗硬复位**
    // （内部分配失败 ⇒ SDK 的 system_show_malloc() 关中断打整张堆图 ⇒ 一次 loop 超时）。
    //
    // 判据来自 556 个可信样本（串口 `[TX] start` + `[API send]` 配对）：
    //   成功的发送，**请求入口** freeHeap 全部 ≥ 4536；崩溃那次是 3680 ⇒ 下界取中间值。
    //   ⚠️ 门槛按 freeHeap 看是**单调**的，按 maxFreeBlock 看**不单调**
    //   （blk=2328 能跑通、blk=2744 崩）⇒ 只能用 freeHeap，⛔ 别改成抬 maxBlock。
    //
    // ⛔⛔ **只能用在"请求入口"（payload 刚到、还没解析），绝不能挪到 IRForwarder::sendSequence()。**
    //   实测（`tools/serial_v38.log`，556 个样本）：发射时（sendSequence 入口）的堆
    //   比请求入口**低约 1.4 KB** —— min 3120 / p5 4200 / p50 4504，
    //   而其中 8 次 heap<4000 的发送**全部成功**（`[TX] end ms=28`，入口 heap 4536~5384）。
    //   ⇒ 把 4096 挪到发射点会把"入口 5000 左右、本来完全正常"的发送误杀
    //   （用户看到的是"点发送没反应"）。这条是踩过才写下的，别再犯。
    //
    // tag: 串口前缀（呼应各入口自己的埋点，如 "API send" / "SmartIR"）
    // 返回 false 表示**应当拒绝本次发射**（调用方返回错误，不要继续往下分配）。
    static bool hasSendHeadroom(const char* tag);
    
    // JSON大小估算函数
    // baseSize: 基础开销（通常256字节）
    // itemCount: 项目数量
    // itemSize: 每个项目的平均大小（字节）
    // 返回估算的JSON文档大小
    static size_t calculateJsonSize(size_t baseSize, int itemCount, size_t itemSize);
    
    // 根据规则数量计算JSON大小
    static size_t calculateRulesJsonSize(int ruleCount);
    
    // 根据日志数量计算JSON大小
    static size_t calculateLogsJsonSize(int logCount);
    
    // 根据信号数量计算JSON大小
    static size_t calculateSignalsJsonSize(int signalCount);
};

#endif // MEMORY_MONITOR_H

