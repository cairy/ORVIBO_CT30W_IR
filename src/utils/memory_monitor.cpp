#include "memory_monitor.h"

// ESP8266 堆大小（动态获取）
// 在启动时记录初始空闲堆，用于计算总堆大小
static size_t cachedHeapSize = 0;
static size_t initialFreeHeap = 0;
static bool initialized = false;

// 初始化内存监控（在启动时调用）
void MemoryMonitor::init() {
    if (!initialized) {
        // 记录启动时的空闲堆（此时内存使用最少）
        initialFreeHeap = ESP.getFreeHeap();
        
        // ESP8266的堆大小 = 初始空闲堆 + 已使用的堆
        // 通过多次采样获取更准确的值
        size_t maxFreeHeap = initialFreeHeap;
        for (int i = 0; i < 5; i++) {
            size_t currentFree = ESP.getFreeHeap();
            if (currentFree > maxFreeHeap) {
                maxFreeHeap = currentFree;
            }
            delay(10);
        }
        
        // 估算总堆大小：初始空闲堆 + 估算已使用堆（约10-20KB）
        // 更准确的方法：初始空闲堆通常接近总堆大小（启动时使用最少）
        // 但考虑到启动时已经分配了一些内存，我们使用一个保守的估算
        cachedHeapSize = maxFreeHeap + 10000;  // 初始空闲堆 + 10KB缓冲
        
        // 限制在合理范围内（ESP8266典型堆大小约50-80KB）
        if (cachedHeapSize < 50000) cachedHeapSize = 50000;
        if (cachedHeapSize > 90000) cachedHeapSize = 90000;
        
        initialized = true;
        
        Serial.printf("[内存监控] 初始化完成 - 初始空闲堆: %d bytes (%.2f KB), 估算总堆: %d bytes (%.2f KB)\n",
                     initialFreeHeap, initialFreeHeap / 1024.0f,
                     cachedHeapSize, cachedHeapSize / 1024.0f);
    }
}

// 获取实际堆大小（动态计算）
static size_t getHeapSize() {
    if (cachedHeapSize == 0) {
        // 如果未初始化，使用默认值
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

float MemoryMonitor::getFragmentationPercent() {
    size_t freeHeap = getFreeHeap();
    size_t maxBlock = getLargestFreeBlock();
    
    if (freeHeap == 0) return 0.0f;
    
    // 碎片率 = (总空闲内存 - 最大连续块) / 总空闲内存 * 100
    size_t fragmented = freeHeap - maxBlock;
    return (float)fragmented * 100.0f / (float)freeHeap;
}

bool MemoryMonitor::isMemoryUsageHigh(float thresholdPercent) {
    return getUsagePercent() > thresholdPercent;
}

void MemoryMonitor::printMemoryInfo() {
    size_t heapSize = getHeapSize();
    size_t freeHeap = getFreeHeap();
    size_t maxBlock = getLargestFreeBlock();
    float usagePercent = getUsagePercent();
    float fragPercent = getFragmentationPercent();
    
    Serial.println("=== 内存使用情况 ===");
    Serial.printf("总堆内存: %u 字节 (%.2f KB)\n", heapSize, heapSize / 1024.0f);
    Serial.printf("空闲堆内存: %u 字节 (%.2f KB)\n", freeHeap, freeHeap / 1024.0f);
    Serial.printf("已使用堆内存: %u 字节 (%.2f KB)\n", getUsedHeap(), getUsedHeap() / 1024.0f);
    Serial.printf("最大连续空闲块: %u 字节 (%.2f KB)\n", maxBlock, maxBlock / 1024.0f);
    Serial.printf("内存使用率: %.2f%%\n", usagePercent);
    Serial.printf("内存碎片率: %.2f%%\n", fragPercent);
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

bool MemoryMonitor::checkMemory(size_t requiredBytes, float maxUsagePercent) {
    size_t freeHeap = getFreeHeap();
    size_t maxBlock = getLargestFreeBlock();
    float usagePercent = getUsagePercent();
    
    // 优先检查实际可用内存，而不是百分比
    // 如果空闲内存足够（大于2KB）且最大连续块足够，即使使用率高也允许请求
    if (freeHeap >= 2048 && maxBlock >= requiredBytes + 512) {
        return true;
    }
    
    // 如果空闲内存不足，再检查百分比
    if (usagePercent > maxUsagePercent) {
        return false;
    }
    
    // 空闲内存小于512字节时拒绝请求
    if (freeHeap < 512) {
        return false;
    }
    
    // 最大连续块小于所需大小+256字节安全余量时拒绝请求
    if (maxBlock < requiredBytes + 256) {
        return false;
    }
    
    return true;
}

size_t MemoryMonitor::calculateJsonSize(size_t baseSize, int itemCount, size_t itemSize) {
    // 基础开销 + 项目数量 * 每个项目大小
    size_t size = baseSize + (itemCount * itemSize);
    
    // 限制最大大小（避免内存溢出）
    // 增加到16384以支持更多规则（ESP8266通常有足够内存）
    if (size > 16384) size = 16384;
    
    // 确保最小大小
    if (size < 256) size = 256;
    
    return size;
}

size_t MemoryMonitor::calculateRulesJsonSize(int ruleCount) {
    // 每条规则约200字节基础开销 + 每个B信号约100字节
    // 假设平均每条规则有2个B信号
    // 注意：使用hex格式时，字符串会更长，需要更多空间
    // 实际测试：5条规则（包含电源规则有3个B信号）需要约3000-4000字节
    size_t baseSize = 512;  // 增加基础开销
    size_t itemSize = 300 + (2 * 120);  // 平均每条规则540字节（考虑hex格式、对象开销和name字段）
    size_t size = baseSize + (ruleCount * itemSize);
    
    // 限制最大大小（增加到16384以支持更多规则）
    if (size > 16384) size = 16384;
    
    // 确保最小大小
    if (size < 2048) size = 2048;
    
    return size;
}

size_t MemoryMonitor::calculateLogsJsonSize(int logCount) {
    // 每条日志约108字节（包含时间戳、类型、内容）
    size_t baseSize = 256;
    size_t itemSize = 108;
    return calculateJsonSize(baseSize, logCount, itemSize);
}

size_t MemoryMonitor::calculateSignalsJsonSize(int signalCount) {
    // 每个信号约100字节（包含时间戳、匹配状态、信号信息）
    size_t baseSize = 256;
    size_t itemSize = 100;
    return calculateJsonSize(baseSize, signalCount, itemSize);
}
