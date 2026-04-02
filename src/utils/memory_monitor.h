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

