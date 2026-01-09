#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include <LittleFS.h>

// LittleFS 存储工具函数
class Storage {
public:
    static bool init();
    static bool saveFile(const String& path, const String& content);
    static String readFile(const String& path);
    static bool fileExists(const String& path);
    static bool deleteFile(const String& path);
    static bool format();
};

#endif // STORAGE_H

