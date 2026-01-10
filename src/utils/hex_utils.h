#ifndef HEX_UTILS_H
#define HEX_UTILS_H

#include <Arduino.h>

// 将uint32_t转换为十六进制字符串（格式：0xXXXXXXXX，8位十六进制，大写）
String uint32ToHexString(uint32_t value);

// 从十六进制字符串解析uint32_t
// 支持格式："0x011B26D9" 或 "011B26D9"（带或不带0x前缀）
// 如果解析失败返回0
uint32_t parseUint32FromHexString(const char* hexStr);

#endif // HEX_UTILS_H
