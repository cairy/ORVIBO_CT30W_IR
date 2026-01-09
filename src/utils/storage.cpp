#include "storage.h"

bool Storage::init() {
    return LittleFS.begin();
}

bool Storage::saveFile(const String& path, const String& content) {
    File file = LittleFS.open(path, "w");
    if (!file) {
        return false;
    }
    
    file.print(content);
    file.close();
    return true;
}

String Storage::readFile(const String& path) {
    if (!LittleFS.exists(path)) {
        return String();
    }
    
    File file = LittleFS.open(path, "r");
    if (!file) {
        return String();
    }
    
    String content = file.readString();
    file.close();
    return content;
}

bool Storage::fileExists(const String& path) {
    return LittleFS.exists(path);
}

bool Storage::deleteFile(const String& path) {
    return LittleFS.remove(path);
}

bool Storage::format() {
    return LittleFS.format();
}

