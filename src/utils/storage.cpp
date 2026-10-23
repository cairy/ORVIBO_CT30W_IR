#include "storage.h"

bool Storage::init() {
    // ⚠️ 必须显式关掉 autoFormat。core 的 LittleFS::begin() 是
    //   `if (_mounted) return true; if (_tryMount()) return true;
    //    if (!_cfg._autoFormat || !format()) return false; return _tryMount();`
    // 而 LittleFSConfig 默认 `_autoFormat = true` ⇒ **挂载不上就静默格式化并成功返回**。
    // 后果：main.cpp 那段"重试 3 次 → 格式化 → 仍失败就主动重启"永远不执行，
    // 而且用户的 WiFi/MQTT 配置与 20 条规则在无人知情、没有任何提示的情况下被清空
    // （串口连"配置已清空，请重新配网"这句都不会打）。
    // 关掉之后，格式化只走 Storage::format() 这一条显式路径 —— 由 main.cpp 决定，
    // 且会明确打日志。
    LittleFSConfig cfg(false);
    LittleFS.setConfig(cfg);
    return LittleFS.begin();
}

bool Storage::saveFile(const String& path, const String& content) {
    File file = LittleFS.open(path, "w");
    if (!file) {
        return false;
    }

    // ⚠️ 必须校验实际写入字节数：以前丢弃 print() 的返回值，flash 写满/坏块时
    // 只写了一半也返回 true。而上层的 "/save 已保存"、NetworkConfigManager::save()
    // 全都信这个返回值 ⇒ 用户被告知成功，重启后配置没了。
    size_t written = file.print(content);
    file.close();
    return (written == content.length());
}

bool Storage::saveFileAtomic(const String& path, const String& content) {
    // 「临时文件 + rename」：任何一步失败都保留原文件，绝不留下半截 JSON。
    // 规则文件（ir_maps.json）早就这么写了，但 WiFi/MQTT 配置与 devices.json
    // 没有 —— 而恰恰是 WiFi 配置写坏了设备会直接退 AP 模式。
    char tmp[48];
    if (path.length() + 5 >= sizeof(tmp)) {
        return saveFile(path, content);   // 路径过长，退化为普通写
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path.c_str());

    if (!saveFile(tmp, content)) {
        LittleFS.remove(tmp);
        return false;
    }
    LittleFS.remove(path);
    if (!LittleFS.rename(tmp, path)) {
        LittleFS.remove(tmp);
        return false;
    }
    return true;
}

bool Storage::saveJsonAtomic(const String& path, const JsonDocument& doc) {
    // 与 saveFileAtomic 同语义（tmp + rename），只是把"内容"换成"文档"：
    // serializeJson 直接写 File，中间不产生整份 String。
    char tmp[48];
    if (path.length() + 5 >= sizeof(tmp)) {
        return false;   // 路径过长：这里没有 String 版本可退化，直接判失败
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path.c_str());

    File file = LittleFS.open(tmp, "w");
    if (!file) {
        return false;
    }
    size_t written = serializeJson(doc, file);
    file.close();
    if (written == 0) {
        // 一个字节都没落地（flash 满 / 句柄失效）—— 与 saveFile 同一个理由：
        // 不校验的话 `print()` 只写一半也会"成功"。
        LittleFS.remove(tmp);
        return false;
    }
    LittleFS.remove(path);
    if (!LittleFS.rename(tmp, path)) {
        LittleFS.remove(tmp);
        return false;
    }
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

// ── 分片写入（v1.3.42）──────────────────────────────────────────────────

bool Storage::appendFile(const String& path, const uint8_t* data, size_t len, size_t& outSize) {
    if (data == nullptr || len == 0) return false;

    File file = LittleFS.open(path, "a");   // "a"：不存在则创建，存在则定位到末尾
    if (!file) {
        return false;
    }
    // ⚠️ "a" 模式下 file.size() 返回的是**打开那一刻**的已有大小（LittleFS 会 stat）。
    // 用"已有 + 本次"自己算总长，别依赖写完后再读 size —— 写入在 close 前可能还在缓存里。
    const size_t before = file.size();
    const size_t written = file.write(data, len);
    file.close();

    if (written != len) {
        // 写了一半：不动文件，交给调用方决定（分片路径会删掉 tmp 重来）
        return false;
    }
    outSize = before + written;
    return true;
}

size_t Storage::fileSize(const String& path) {
    if (!LittleFS.exists(path)) return 0;
    File file = LittleFS.open(path, "r");
    if (!file) return 0;
    const size_t n = file.size();
    file.close();
    return n;
}

bool Storage::commitFile(const String& tmpPath, const String& finalPath) {
    // 与 saveFileAtomic 的收尾一致：先删目标再改名（LittleFS::rename 在目标存在时会失败）。
    LittleFS.remove(finalPath);
    if (!LittleFS.rename(tmpPath, finalPath)) {
        LittleFS.remove(tmpPath);
        return false;
    }
    return true;
}

