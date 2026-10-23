#ifndef TIMING_SCRATCH_H
#define TIMING_SCRATCH_H

#include "hardware/ir_receiver.h"
#include "config/constants.h"

// ============================================================================
// 共享的「时序临时缓冲」
//
// 背景：固件里原先有多处「用完即弃」的 IRTiming 暂存数组——解析字符串格式规则、
// 接收学习信号、编译待转发信号——它们各自常驻 .bss，且从不在同一时刻使用。
// 本机空闲堆只有几 KB（每 1 B 的 .bss 就是 1 B 的可用堆），故改为共用同一块。
//
// 使用约定：
//   1. 拿到的指针只在**当前函数内**使用，不得跨调用持有——下一个使用者会覆盖它；
//   2. 需要「留到后面用」的数据必须拷出去（UnifiedSignal / SignalSequence 都是深拷贝）。
//
// 容量：见 TIMING_SCRATCH_CAPACITY（= MAX_LEARN_TIMING_COUNT 点 × 8 B）。
// ============================================================================

#define TIMING_SCRATCH_CAPACITY MAX_LEARN_TIMING_COUNT

IRTiming* timingScratch();

#endif // TIMING_SCRATCH_H
