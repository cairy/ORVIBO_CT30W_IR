#include "utils/timing_scratch.h"

// 全局唯一的实体：所有「临时时序」使用者共享这一块。
// 定义在 .cpp 里（而非头文件 inline/static），确保整份固件只有一份，不被各编译单元复制。
static IRTiming s_timingScratch[TIMING_SCRATCH_CAPACITY];

IRTiming* timingScratch() {
    return s_timingScratch;
}
