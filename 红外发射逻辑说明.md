# 红外发射逻辑说明

## 核心思路

发射逻辑统一通过 `sendSequence()` 处理信号序列，所有格式最终都转换为`SignalSequence`格式发送。使用序列中信号的`repeatCount`字段控制重复次数，不再使用外部`repeatCount`参数。

## 发射入口

### 1. 转发模式（`forwardSignal`）
- **位置**：`src/ir_core/ir_forwarder.cpp:139`
- **用途**：接收A信号，查找映射规则，转发B信号
- **流程**：
  - 简化逻辑：直接查找规则，延时1ms后转发
  - 不再区分完整信号和重复码
  - 不再保存上一个信号状态

### 2. API直接发送（`parseAndSendSignal`）
- **位置**：`src/ir_core/ir_forwarder.cpp:513`
- **用途**：HTTP API和MQTT接口直接发送信号
- **支持格式**：
  - **JSON格式**：序列格式JSON（`{"signals":[...], "intervals":[...]}`）
  - **字符串格式**：字符串序列格式（支持单个信号和序列）
    - Base64格式：`ABC123==|2` 或 `ABC123==@30;DEF456==|2`
    - 数组格式：`[8991,-4546]|3` 或 `[8991,-4546]@50;[1000,-2000]|1`
    - 协议格式：`1,0xFF00FF00,2` 或 `1,0xFF00FF00@30;2,0x12345678|2`

### 3. 统一发送接口（`sendSequence`）
- **位置**：`src/ir_core/ir_forwarder.cpp:296`
- **用途**：所有发射的最终统一接口
- **参数**：`sequence`（信号序列）
- **repeatCount**：使用序列中第一个信号的`repeatCount`字段（如果>1，重复发送整个序列）

## 发射流程（`sendSequence`）

### 1. 信号类型判断
- **时序码信号**：`signal.useTiming == true`
- **NEC协议**：`protocol == 1 && !useTiming`
- **其他协议**：使用时序生成器生成时序

### 2. 时序码信号发射
- 从Base64加载时序码到内存
- 循环发送：外层循环`repeatCount`次，内层循环序列中的每个信号
- 信号间隔：根据`intervals`数组精确延时
- 重复间隔：每次重复后延时100ms（最后一次不延时）

### 3. NEC协议单个信号发射
- **重复码**：直接调用`sendNECRepeat()`
- **完整信号**：如果信号自身的`repeatCount>1`，发送1次完整信号 + (repeatCount-1)次重复码
- **重复码间隔**：110ms（NEC协议标准）

### 4. 其他协议/序列发射
- 使用时序生成器编译信号为时序码
- 循环发送：外层循环`repeatCount`次，内层循环序列中的每个信号
- 信号间隔和重复间隔同时序码信号

## 关键实现细节

### 间隔处理
- **第一个信号前间隔**：`hasFirstInterval == true`时，`intervals[0]`是第一个信号前的间隔
- **信号间间隔**：
  - `hasFirstInterval`：`intervals[i+1]`是`signals[i]`和`signals[i+1]`之间的间隔
  - 标准模式：`intervals[i]`是`signals[i]`和`signals[i+1]`之间的间隔

### 重复码处理
- **转发模式**：已简化，不再检测重复码
- **NEC协议**：如果信号自身的`repeatCount>1`，发送1次完整信号+(repeatCount-1)次重复码，重复码间隔110ms（协议标准）
- **其他协议**：默认100ms重复间隔

### 时序精度
- 使用`PreciseTimer`实现精准延时（误差≤20ms）
- 发射时关闭中断确保时序精度（最多10ms）
- 每100个时序点检查一次，避免长时间阻塞

## 与学习逻辑的一致性检查

### ✅ 一致的部分
1. **信号格式**：统一使用`UnifiedSignal`和`SignalSequence`
2. **序列结构**：支持多个信号+间隔
3. **repeatCount使用**：统一使用信号自身的`repeatCount`字段
4. **格式统一**：学习、映射、发送都使用相同的Sequence格式

### ✅ 已统一的部分
1. **repeatCount使用**：
   - **学习时**：重复码会增加信号自身的`repeatCount`（`signal.repeatCount++`）
   - **发射时**：使用序列中第一个信号的`repeatCount`字段（如果>1，重复发送整个序列）
   - **一致性**：已统一，学习时保存的`repeatCount`会在发射时使用

2. **NEC重复码处理**：
   - **学习时**：检测到重复码增加`repeatCount`，不添加新信号
   - **发射时**：NEC协议单个信号，如果信号自身的`repeatCount>1`，发送1次完整信号+(repeatCount-1)次重复码
   - **一致性**：逻辑一致，使用信号自身的`repeatCount`

## 发射位置汇总

1. **`main.cpp`**：接收信号后调用`forwardSignal()`
2. **`ha_mqtt_handler.cpp`**：MQTT消息处理，调用`parseAndSendSignal()`
3. **`api_handlers.cpp`**：HTTP API处理，调用`parseAndSendSignal()`
4. **`ir_forwarder.cpp`**：
   - `forwardSignal()` → `executeForward()` → `sendSequence()`
   - `parseAndSendSignal()` → `sendSequence()`
   - `sendSignal()` → `sendSequence()`

## 关键修改点

### 统一Sequence格式
- **所有输入格式**：JSON和字符串格式都转换为`SignalSequence`格式
- **字符串序列格式**：支持`signal1[@interval1];signal2[@interval2]|repeatCount`格式
- **第一个信号前间隔**：支持`@50;signal1@30;signal2|1`格式

### repeatCount使用逻辑
- **文件**：`src/ir_core/ir_forwarder.cpp`
- **函数**：`sendSequence()`
- **位置**：第296行
- **实现**：使用序列中第一个信号的`repeatCount`字段（如果>1，重复发送整个序列）

### 转发逻辑简化
- **文件**：`src/ir_core/ir_forwarder.cpp`
- **函数**：`forwardSignal()`
- **修改**：去掉重复码检测，延时从40ms改为1ms

### NEC重复码处理
- **文件**：`src/ir_core/ir_forwarder.cpp`
- **位置**：第358-382行
- **实现**：如果信号自身的`repeatCount>1`，发送1次完整信号+(repeatCount-1)次重复码，间隔110ms

## 注意事项

1. **内存管理**：时序码从Base64加载后需要及时释放
2. **中断处理**：发射时关闭中断，但需要定期检查避免长时间阻塞
3. **延时精度**：使用`PreciseTimer`确保信号间隔准确
4. **格式一致性**：学习、映射、发送都使用统一的Sequence格式
