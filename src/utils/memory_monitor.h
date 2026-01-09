#ifndef MEMORY_MONITOR_H
#define MEMORY_MONITOR_H

#include <Arduino.h>

// 内存监控工具类
class MemoryMonitor {
public:
    // 初始化内存监控（在启动时调用，记录初始堆大小）
    static void init();
    
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
};

#endif // MEMORY_MONITOR_H

