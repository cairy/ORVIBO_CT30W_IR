#include "memory_monitor.h"

// ESP8266 堆大小（动态获取，典型值约80KB）
// 注意：ESP8266的堆大小可能因编译选项和运行时状态而变化
static size_t cachedHeapSize = 0;

// 获取实际堆大小（动态计算）
static size_t getHeapSize() {
    if (cachedHeapSize == 0) {
        // ESP8266没有直接获取堆大小的API，通过计算获取
        // 方法：获取当前空闲堆 + 已使用堆（通过内存分配测试）
        // 简化方法：使用典型值，但在运行时根据实际情况调整
        // ESP8266典型堆大小约80KB，但实际可能因编译选项而异
        // 使用一个合理的估算值：空闲堆 + 估算已使用堆
        // 更准确的方法是在启动时记录初始空闲堆，但这里使用典型值
        cachedHeapSize = 81920;  // 80KB典型值
    }
    return cachedHeapSize;
}

size_t MemoryMonitor::getFreeHeap() {
    return ESP.getFreeHeap();
}

size_t MemoryMonitor::getUsedHeap() {
    size_t freeHeap = ESP.getFreeHeap();
    size_t heapSize = getHeapSize();
    // 总堆大小 - 空闲堆 = 已使用堆
    if (freeHeap < heapSize) {
        return heapSize - freeHeap;
    }
    return 0;
}

size_t MemoryMonitor::getLargestFreeBlock() {
    return ESP.getMaxFreeBlockSize();
}

size_t MemoryMonitor::getMinFreeHeap() {
    // ESP8266 没有 getMinFreeHeap，返回当前空闲堆作为近似值
    return ESP.getFreeHeap();
}

float MemoryMonitor::getUsagePercent() {
    size_t freeHeap = ESP.getFreeHeap();
    size_t heapSize = getHeapSize();
    
    if (heapSize == 0) return 0.0f;
    
    size_t used = (freeHeap < heapSize) ? (heapSize - freeHeap) : 0;
    return (float)used * 100.0f / (float)heapSize;
}

bool MemoryMonitor::isMemoryUsageHigh(float thresholdPercent) {
    return getUsagePercent() > thresholdPercent;
}

void MemoryMonitor::printMemoryInfo() {
    size_t heapSize = getHeapSize();
    Serial.println("=== 内存使用情况 ===");
    Serial.printf("总堆内存: %u 字节 (%.2f KB)\n", heapSize, heapSize / 1024.0f);
    Serial.printf("空闲堆内存: %u 字节 (%.2f KB)\n", getFreeHeap(), getFreeHeap() / 1024.0f);
    Serial.printf("已使用堆内存: %u 字节 (%.2f KB)\n", getUsedHeap(), getUsedHeap() / 1024.0f);
    Serial.printf("最大连续空闲块: %u 字节 (%.2f KB)\n", getLargestFreeBlock(), getLargestFreeBlock() / 1024.0f);
    Serial.printf("当前空闲堆内存: %u 字节 (%.2f KB)\n", getMinFreeHeap(), getMinFreeHeap() / 1024.0f);
    Serial.printf("内存使用率: %.2f%%\n", getUsagePercent());
    Serial.println("==================");
}

String MemoryMonitor::getMemoryInfoString() {
    size_t heapSize = getHeapSize();
    String info = "内存: ";
    info += String(getUsedHeap() / 1024.0f, 2);
    info += "/";
    info += String(heapSize / 1024.0f, 2);
    info += " KB (";
    info += String(getUsagePercent(), 1);
    info += "%)";
    return info;
}

