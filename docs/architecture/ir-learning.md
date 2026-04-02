# 红外学习逻辑说明

## 核心思路

学习模式通过接收红外时序数据，解析为统一信号格式，并支持重复码检测和信号序列识别。

## 主要流程

### 1. 启动学习
- 调用 `startDeviceLearn(deviceName, commandNames, timeoutSeconds)`
- 检查内存，动态分配 `LearnedDevice`
- 解析指令名称列表（逗号分隔）
- 禁用库接收器，启用自定义接收器

### 2. 信号接收与处理（`processDeviceLearnSignal`）

#### 2.1 信号解析
- 优先尝试解析为标准协议（NEC/RC5/RC6/Sony/Samsung）
- 解析失败则使用时序码，编码为Base64存储
- 两种格式都保存在 `UnifiedSignal` 中

#### 2.2 重复码检测（新增）
- **时机**：在判断是否加入序列之前
- **条件**：
  - 序列中已有信号（`signalCount > 0`）
  - 当前信号与上一个信号相同
  - 间隔在 100-120ms 范围内（`REPEAT_CODE_INTERVAL_MIN/MAX_MS`）
- **处理**：增加上一个信号的 `repeatCount`，不添加新信号
- **目的**：正确识别NEC等协议的重复码，避免重复添加相同信号

#### 2.3 序列检测
- 计算与上一个信号的时间间隔
- **加入序列**：间隔 < 1000ms (`SIGNAL_INTERVAL_MAX_MS`) 且序列未满（最多5个信号）
- **新序列**：间隔 >= 1000ms 或序列已满，清空并开始新序列

#### 2.4 完成当前指令
- 标记指令有效
- 更新指令计数
- 如果所有指令学习完成，自动结束学习

### 3. 超时处理
- 每次 `update()` 检查学习超时
- 超时自动调用 `stopDeviceLearn()`

### 4. 输出格式
- **Base64格式**：默认格式，兼容SmartIR
- **SignalID格式**：协议信号优先，支持序列格式
- **数组格式**：时序码数组字符串

## 关键数据结构

```cpp
struct LearnedCommand {
    char name[MAX_COMMAND_NAME_LEN];
    SignalSequence sequence;  // 信号序列（支持多个信号+间隔）
    bool valid;
};

struct SignalSequence {
    UnifiedSignal signals[MAX_B_SIGNALS_PER_RULE];  // 最多5个信号
    uint8_t signalCount;
    uint16_t intervals[MAX_B_SIGNALS_PER_RULE];    // 信号间隔
    // ...
};
```

## 重要配置参数

- `MAX_LEARN_COMMANDS`: 5（最大指令数）
- `MAX_LEARN_TIMING_COUNT`: 200（每个信号最大时序点数）
- `SIGNAL_INTERVAL_MAX_MS`: 1000（序列检测最大间隔）
- `REPEAT_CODE_INTERVAL_MIN_MS`: 100（重复码最小间隔）
- `REPEAT_CODE_INTERVAL_MAX_MS`: 120（重复码最大间隔）
- `SIGNAL_REPEAT_MAX`: 10（最大重复次数）

## 修改要点

### 重复码检测逻辑位置
- 文件：`src/ir_core/ir_learner.cpp`
- 函数：`processDeviceLearnSignal()`
- 位置：第368-385行（在序列检测之前）

### 常量定义位置
- 文件：`src/config/constants.h`
- 位置：信号处理配置区域（第34-35行）

## 注意事项

1. 内存管理：学习时动态分配，完成后保留数据用于格式转换
2. 时序码存储：使用Base64字符串，节省内存
3. 重复码检测：只在序列已有信号时进行，避免误判
4. 序列限制：最多5个信号，超过则开始新序列
