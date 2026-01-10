#include "hex_utils.h"

String uint32ToHexString(uint32_t value) {
    char hexStr[11]; // "0x" + 8位十六进制 + '\0' = 11字节
    snprintf(hexStr, sizeof(hexStr), "0x%08X", value);
    return String(hexStr);
}

uint32_t parseUint32FromHexString(const char* hexStr) {
    if (hexStr == nullptr) {
        return 0;
    }
    
    // 跳过前导空格
    while (*hexStr == ' ' || *hexStr == '\t') {
        hexStr++;
    }
    
    // 检查是否有0x前缀
    const char* start = hexStr;
    if (hexStr[0] == '0' && (hexStr[1] == 'x' || hexStr[1] == 'X')) {
        start = hexStr + 2;
    }
    
    // 解析十六进制字符串
    uint32_t result = 0;
    for (const char* p = start; *p != '\0'; p++) {
        char c = *p;
        uint8_t digit = 0;
        
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else {
            // 遇到非十六进制字符，停止解析
            break;
        }
        
        result = (result << 4) | digit;
    }
    
    return result;
}
