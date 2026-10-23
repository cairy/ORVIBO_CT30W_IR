#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// LittleFS 存储工具函数
class Storage {
public:
    static bool init();
    // 写入并校验实际字节数（写一半会返回 false，不再谎报成功）
    static bool saveFile(const String& path, const String& content);
    // 临时文件 + rename 的原子写入：失败时原文件保持不动。
    // 关键配置（WiFi/MQTT、devices.json）应当用它，而不是 saveFile。
    static bool saveFileAtomic(const String& path, const String& content);
    // ⭐ v1.3.47：直接把 JsonDocument **序列化写盘**（同样是"临时文件 + rename"）。
    //   存在的理由：调用方若先在 RAM 里拼出整份 String 再交给 saveFileAtomic，
    //   峰值内存 = 文档池 + 一份等长的 String（"刚学完"时最缺的正是这块连续内存）。
    //   走这条只需文档池本身。返回 false = 一个字节都没写进去（不谎报成功）。
    static bool saveJsonAtomic(const String& path, const JsonDocument& doc);
    static String readFile(const String& path);
    static bool fileExists(const String& path);
    static bool deleteFile(const String& path);
    static bool format();

    // ── 分片写入（v1.3.42，服务于设备配置的"分片提交"）──────────────────────
    // 目的：让设备端 RAM 占用与**配置总大小无关**。整份配置一次 POST 时，
    // ESP8266WebServer 收 body 要一次性读完（读不满即丢连接），堆紧时根本收不下；
    // 改成逐片追加后，每片只需约 3×片长的连续块。
    // 用法：deleteFile(tmp) → 逐片 appendFile(tmp) → commitFile(tmp, final)。
    // 追加写：文件不存在则创建。成功时把**追加后的总字节数**写入 outSize。
    // ⚠️ 必须校验真实写入字节数并在不足时报 false —— 与 saveFile 同一个理由：
    //    flash 写满时 `print()` 只写一半也返回成功过。
    static bool appendFile(const String& path, const uint8_t* data, size_t len, size_t& outSize);
    static size_t fileSize(const String& path);
    // rename(tmp, final)：先删 final 再改名，失败时删除 tmp（与 saveFileAtomic 同语义）。
    static bool commitFile(const String& tmpPath, const String& finalPath);
};

#endif // STORAGE_H

