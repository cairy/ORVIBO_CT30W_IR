#include "crash_record.h"

#include <user_interface.h>   // rst_info / REASON_EXCEPTION_RST / system_rtc_mem_*
#include "config/constants.h"
#include "utils/storage.h"

// UMM 记的"最后一次失败的分配"。核心的 postmortem 也是从这里取的
// （core_esp8266_postmortem.cpp:68 与 :258-265）——内存耗尽崩掉时，
// 这两个值直接告诉我们"要多大的一块、谁要的"，正是 Exception 29 最缺的线索。
extern "C" {
extern void* umm_last_fail_alloc_addr;
extern int   umm_last_fail_alloc_size;
}

// ── 记录布局（全部 uint32，共 52 个字）──────────────────────────────────────
//   0  magic
//   1  reason        rst_info.reason（2=exception / 3=软看门狗 / 1=硬看门狗）
//   2  exccause      rst_info.exccause（29=load 到非法地址，常见于 OOM 后空指针）
//   3  epc1          ⭐ 崩在哪个地址 —— 最有价值的一格，用 .elf 反解成函数名
//   4  excvaddr      出错时访问的地址
//   5  freeHeap      崩那一刻的空闲堆
//   6  uptimeMs      崩那一刻的 uptime（毫秒）
//   7  failAddr      OOM 时 = 那次失败分配的调用者地址
//   8  failSize      OOM 时 = 那次失败分配要的字节数
//   9  stackStart    栈区间起点（核心 print_stack 的起点，便于和人肉 dump 对照）
//  10  checksum      word 1..9 的 XOR
//  11..51 栈字（41 个）
static const uint32_t CRASH_MAGIC = 0xC5A5C0DE;

// ⚠️ 用 static 缓冲、不用栈上的数组：崩溃当刻栈可能已经见底（"栈溢出"本身就是
//    一大类崩溃原因），这时候再在栈上开 208 B 会帮倒忙。
static uint32_t s_buf[CrashRecord::RTC_WORDS];

// 只做两件事：填缓冲、写 RTC。**自包含、纯 RAM**（见头文件的铁律 1）。
static void writeRecord(uint32_t reason, uint32_t exccause, uint32_t epc1, uint32_t excvaddr,
                        uint32_t freeHeap, uint32_t uptimeMs,
                        uint32_t failAddr, uint32_t failSize,
                        uint32_t stackStart, const uint32_t* stackWords, uint32_t stackWordCount) {
    s_buf[0]  = CRASH_MAGIC;
    s_buf[1]  = reason;
    s_buf[2]  = exccause;
    s_buf[3]  = epc1;
    s_buf[4]  = excvaddr;
    s_buf[5]  = freeHeap;
    s_buf[6]  = uptimeMs;
    s_buf[7]  = failAddr;
    s_buf[8]  = failSize;
    s_buf[9]  = stackStart;

    uint32_t chk = 0;
    for (uint32_t i = 1; i <= 9; i++) {
        chk ^= s_buf[i];
    }
    s_buf[10] = chk;

    for (uint32_t i = 0; i < CrashRecord::STACK_WORDS; i++) {
        s_buf[CrashRecord::HEADER_WORDS + i] = (i < stackWordCount && stackWords) ? stackWords[i] : 0;
    }

    ESP.rtcUserMemoryWrite(CrashRecord::RTC_OFFSET, s_buf, CrashRecord::RTC_WORDS * 4);
}

// ── 核心的崩溃回调（覆盖弱符号）──────────────────────────────────────────────
// 签名与 core_esp8266_postmortem.cpp:83 的弱符号**必须逐字一致**，否则链接的是核心那份空实现。
// stack / stack_end 就是紧接着要被打印到串口的那段 `>>>stack>>>` 区间。
extern "C" void custom_crash_callback(struct rst_info* rst, uint32_t stack, uint32_t stack_end) {
    const uint32_t reason    = rst ? (uint32_t)rst->reason    : 0;
    const uint32_t exccause  = rst ? (uint32_t)rst->exccause  : 0;
    const uint32_t epc1      = rst ? (uint32_t)rst->epc1      : 0;
    const uint32_t excvaddr  = rst ? (uint32_t)rst->excvaddr  : 0;

    // 只读几个寄存器/计数器，都不依赖中断
    const uint32_t freeHeap  = (uint32_t)ESP.getFreeHeap();
    const uint32_t uptimeMs  = (uint32_t)millis();
    const uint32_t failAddr  = (uint32_t)(uintptr_t)umm_last_fail_alloc_addr;
    const uint32_t failSize  = (uint32_t)umm_last_fail_alloc_size;

    // 栈字从 stack 往上取；越界的一律写 0（宁可少记，不许越界读）
    const uint32_t* sp = (const uint32_t*)(uintptr_t)stack;
    uint32_t avail = 0;
    if (stack_end > stack) {
        avail = (stack_end - stack) / 4;
        if (avail > CrashRecord::STACK_WORDS) {
            avail = CrashRecord::STACK_WORDS;
        }
    }
    writeRecord(reason, exccause, epc1, excvaddr, freeHeap, uptimeMs,
                failAddr, failSize, stack, sp, avail);
}

// ── 自测入口：喂一份假现场（不真的崩）────────────────────────────────────────
void CrashRecord::captureForTest(uint32_t reason, uint32_t exccause,
                                 uint32_t epc1, uint32_t excvaddr,
                                 uint32_t failAddr, uint32_t failSize,
                                 uint32_t stackStart, const uint32_t* stackWords,
                                 uint32_t stackWordCount) {
    writeRecord(reason, exccause, epc1, excvaddr,
                (uint32_t)ESP.getFreeHeap(), (uint32_t)millis(),
                failAddr, failSize, stackStart, stackWords, stackWordCount);
}

// ── 开机：RTC → flash ───────────────────────────────────────────────────────
static void hexWord(String& out, uint32_t v) {
    char tmp[12];
    snprintf(tmp, sizeof(tmp), "0x%08x", (unsigned)v);
    out += tmp;
}

// v1.3.51：这些字面量原本落在 .rodata —— 而本 core 的 .rodata VMA 是 0x3FFE8660
// （**DRAM**，占堆）⇒ 套 PSTR 挪进 flash。返回值当 const char* 用没问题：
// ESP8266 的 flash 可直接按字节读（`%s` / String 拼接都能用）。
static const char* reasonText(uint32_t r) {
    switch (r) {
        case 0: return PSTR("power-on");
        case 1: return PSTR("hardware-watchdog");
        case 2: return PSTR("exception");
        case 3: return PSTR("software-watchdog");
        case 4: return PSTR("software-restart");
        case 5: return PSTR("deep-sleep-awake");
        case 6: return PSTR("external-reset");
        default: return PSTR("unknown");
    }
}

void CrashRecord::collectOnBoot() {
    // ⚠️ v1.3.51 抠 RAM：复用文件作用域的 s_buf，不再另开一份 208 B 的 static 数组。
    //    两者时间上**严格互斥**：s_buf 只在 writeRecord()（崩溃当刻、重启前）被填，
    //    collectOnBoot() 只在开机早期读一次并回写 RTC —— 单线程、永不交叠。
    uint32_t* const buf = s_buf;
    if (!ESP.rtcUserMemoryRead(RTC_OFFSET, buf, sizeof(s_buf))) {
        return;
    }
    if (buf[0] != CRASH_MAGIC) {
        return;   // 没有现场（绝大多数启动都是这一条）
    }

    uint32_t chk = 0;
    for (uint32_t i = 1; i <= 9; i++) {
        chk ^= buf[i];
    }
    if (chk != buf[10]) {
        Serial.println(F("[崩溃留档] RTC 校验不过，判为残留垃圾并清掉"));
        buf[0] = 0;
        ESP.rtcUserMemoryWrite(RTC_OFFSET, buf, 4);
        return;
    }

    // 手工拼 JSON（现场本身就是小对象，不值得为它开 JsonDocument 池）
    String s;
    s.reserve(1024);
    s += F("{\"reason\":");
    s += String((unsigned)buf[1]);
    s += F(",\"reasonText\":\"");
    s += reasonText(buf[1]);
    s += F("\",\"exccause\":");
    s += String((unsigned)buf[2]);
    s += F(",\"epc1\":\"");
    hexWord(s, buf[3]);
    s += F("\",\"excvaddr\":\"");
    hexWord(s, buf[4]);
    s += F("\",\"freeHeap\":");
    s += String((unsigned)buf[5]);
    s += F(",\"uptimeMs\":");
    s += String((unsigned long)buf[6]);
    s += F(",\"failAddr\":\"");
    hexWord(s, buf[7]);
    s += F("\",\"failSize\":");
    s += String((int)buf[8]);
    s += F(",\"stackStart\":\"");
    hexWord(s, buf[9]);
    s += F("\",\"stack\":[");
    for (uint32_t i = 0; i < STACK_WORDS; i++) {
        if (i) {
            s += F(",");
        }
        s += '"';
        hexWord(s, buf[HEADER_WORDS + i]);
        s += '"';
    }
    s += F("]}");

    if (!Storage::saveFileAtomic(CONFIG_FILE_CRASH, s)) {
        // ⭐ 落盘失败**不清 RTC**：留着，下次开机再试（见头文件铁律 3）
        // 格式串用 printf_P/PSTR 放 flash（v1.3.51）：本 core 的 .rodata 落在 DRAM
        // （VMA 0x3FFE8660）⇒ 普通字面量是**吃堆**的，中文串尤其肥。
        Serial.printf_P(PSTR("[崩溃留档] 现场落盘失败（%u B），保留在 RTC 里下次再试\n"),
                        (unsigned)s.length());
        return;
    }

    // 落盘成功才失效 RTC，免得每次普通重启都把同一份现场重写一遍
    buf[0] = 0;
    ESP.rtcUserMemoryWrite(RTC_OFFSET, buf, 4);

    Serial.printf_P(PSTR("[崩溃留档] 已保存上次崩溃现场：reason=%u(%s) exccause=%u epc1=0x%08x "
                         "heap=%u fail=%uB / %u B 记录\n"),
                    (unsigned)buf[1], reasonText(buf[1]), (unsigned)buf[2],
                    (unsigned)buf[3], (unsigned)buf[5], (unsigned)buf[8], (unsigned)s.length());
}

bool CrashRecord::has() {
    return Storage::fileExists(CONFIG_FILE_CRASH);
}

String CrashRecord::readJson() {
    const String s = Storage::readFile(CONFIG_FILE_CRASH);
    if (s.length() == 0) {
        return String("{}");
    }
    return s;
}

void CrashRecord::clear() {
    Storage::deleteFile(CONFIG_FILE_CRASH);
}
