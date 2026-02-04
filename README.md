# ORVIBO-CT30W 红外转发器 - 使用指南

## 一、快速开始

### 环境准备
1. 安装 VS Code 和 PlatformIO IDE 扩展
2. 验证安装：`pio --version`

### 编译与烧录
- **编译**：VS Code 点击 "✓" 或 `Ctrl+Alt+B`，或命令行 `pio run`
- **烧录**：VS Code 点击 "→" 或 `Ctrl+Alt+U`，或命令行 `pio run --target upload`
- **监控**：VS Code 点击 "🔌" 或 `Ctrl+Alt+S`，或命令行 `pio device monitor`

固件位置：`.pio/build/esp12e/firmware.bin`

### 首次配置
设备无 WiFi 配置时自动进入 AP 模式：
- SSID: `ORVIBO-CT30W`，密码: `12345678`
- 访问 `192.168.4.1` 进行配网
- 或使用串口 AT 指令：`AT+WIFI=SSID,密码`、`AT+MQTT=地址,端口,用户,密码`

首次启动会自动初始化默认映射关系（音量+、音量-、静音、电源、AV/TV 等）。

**注意**：所有信号数据（`data` 字段）统一使用十六进制字符串格式。如果设备检测到旧格式数据（数字格式），会自动清空并重新初始化默认映射规则。

## 二、信号参数说明

### 2.1 单个信号（UnifiedSignal）

单个红外信号数据结构包含以下字段：

```json
{
  "protocol": 1,           // 协议类型（1=NEC, 2=RC5, 3=RC6, 4=Sony, 5=Samsung, 0=未知），仅当 useTiming=false 时有效
  "data": "0x00003039",    // 协议数据（32位），仅当 useTiming=false 时有效
  "useTiming": false,      // 是否使用时序码（true=时序码信号，false=协议信号）
  "timingBase64": "",      // 时序码Base64字符串（存储用，Flash中），仅当 useTiming=true 时有效
  "timingCount": 0,        // 时序码数量（时序点的个数），仅当 useTiming=true 时有效
  "repeatCount": 1         // 重复发送次数（默认1），用于控制信号发送的重复次数
}
```

**说明**：
- `useTiming=false` 时，使用 `protocol` 和 `data` 进行匹配和发送（标准协议信号）
- `useTiming=true` 时，使用 `timingBase64` 进行匹配和发送（时序码信号，用于非标准协议或需要精确时序的信号）
- `isProtocol` 字段已移除，可通过 `!useTiming` 推导（即 `isProtocol = !useTiming`）
- 录制的信号、最近接收的信号和规则中的信号可以直接相互拷贝使用

### 2.2 信号序列（SignalSequence）

一个红外命令由信号序列表示，单个信号也是序列（`signalCount=1`）。信号序列结构包含以下字段：

```json
{
  "signals": [             // 信号数组（最多5个信号）
    {
      "protocol": 1,
      "data": "0x00003039",
      "useTiming": false,
      "timingBase64": "",
      "timingCount": 0,
      "repeatCount": 1
    }
  ],
  "intervals": [0, 30, 70]  // 间隔数组（毫秒）
}
```

**说明**：
- **repeatCount控制**：序列中信号的`repeatCount`字段控制重复次数（使用序列中第一个信号的`repeatCount`字段，如果>1，重复发送整个序列）
- **intervals 数组语义**：
  - 如果 `intervals` 数量等于 `signalCount`：`intervals[0]` 表示第一个信号前的间隔，`intervals[i]` (i>0) 表示 `signals[i-1]` 和 `signals[i]` 之间的间隔
  - 如果 `intervals` 数量等于 `signalCount-1`：`intervals[i]` 表示 `signals[i]` 和 `signals[i+1]` 之间的间隔（标准模式）
- **单个信号也是序列**：当 `signalCount=1` 时，`intervals` 可以为空 `[]` 或包含一个值 `[delay]`（表示第一个信号前的间隔）
- **重复间隔**：
  - NEC协议重复码间隔：110ms（NEC协议标准，用于发送重复码）
  - 时序码信号重复间隔：100ms（默认值，用于重复发送时序码信号序列）
- **序列示例**：
  - 单个信号（无第一个信号前间隔）：`{"signals": [{...}], "intervals": []}`
  - 单个信号（有第一个信号前间隔）：`{"signals": [{...}], "intervals": [50]}`（50ms后发送第一个信号）
  - 多个信号（标准模式）：`{"signals": [{...}, {...}], "intervals": [30]}`（两个信号之间间隔30ms）
  - 多个信号（有第一个信号前间隔）：`{"signals": [{...}, {...}], "intervals": [50, 30]}`（50ms后发送第一个信号，两个信号之间间隔30ms）

### 2.3 字符串序列格式

API发送信号支持**字符串序列格式**，用于简洁地表示信号序列。格式规则如下：

**格式规则**：
```
signal1[@interval1];signal2[@interval2];signal3[@interval3]|repeatCount
```

- **`;`** 分隔多个信号
- **`@间隔毫秒数`** 表示与下一个信号的间隔（可选，默认0）
- **`|repeatCount`** 表示序列级别的重复次数（可选，默认1）
- **第一个信号前的间隔**：如果第一个信号段以`@`开头，例如`@50;signal1@30;signal2|1`

**信号格式**：
- **Base64格式**：时序码的Base64编码字符串，例如`ABC123==`
- **数组格式**：时序码数组，例如`[8991,-4546,1000,-500]`
- **协议格式**：`protocol,data`或`protocol,data,repeatCount`，例如`1,0xFF00FF00,2`

**示例**：

1. **单个信号**（向后兼容）：
   - `ABC123==|2` - Base64信号，重复2次
   - `[8991,-4546]|3` - 数组信号，重复3次
   - `1,0xFF00FF00,2` - NEC协议信号，重复2次

2. **两个信号的序列**：
   - `ABC123==@30;DEF456==|2`
   - 表示：信号1 → 等待30ms → 信号2，整个序列重复2次

3. **三个信号的序列**：
   - `[8991,-4546]@50;[1000,-2000]@30;[3000,-4000]|1`
   - 表示：信号1 → 等待50ms → 信号2 → 等待30ms → 信号3

4. **协议格式序列**：
   - `1,0xFF00FF00@30;2,0x12345678@50;1,0xABCDEF00|2`
   - 表示：3个协议信号，带间隔，序列重复2次

5. **混合格式序列**：
   - `ABC123==@30;[8991,-4546]@50;1,0xFF00FF00|1`
   - 表示：Base64信号 → 数组信号 → 协议信号

6. **第一个信号前间隔**：
   - `@50;ABC123==@30;DEF456==|1`
   - 表示：等待50ms → 信号1 → 等待30ms → 信号2

**解析逻辑**：
1. 先提取最后的`|repeatCount`（如果存在）
2. 按`;`分割得到多个信号段
3. 对每个信号段，提取`@间隔`（如果存在）
4. 解析信号部分（Base64/数组/协议格式）
5. 构建SignalSequence对象

**注意事项**：
- 序列级别的`repeatCount`会应用到序列中第一个信号的`repeatCount`字段
- 如果信号自身有`repeatCount`（协议格式中的第三个参数），优先使用信号自身的`repeatCount`
- 第一个信号前的间隔通过`@50;`格式指定，会设置到序列的`intervals[0]`

### 支持的协议

设备支持以下标准红外协议的完整解析和生成：

| 协议 | 协议ID | 说明 | 数据格式 |
|------|--------|------|----------|
| **NEC** | 1 | 最常见的红外协议，支持标准格式和扩展格式 | 32位数据（地址+命令） |
| **RC5** | 2 | 飞利浦RC5协议，使用曼彻斯特编码 | 13位数据（控制位+地址+命令） |
| **RC6** | 3 | 飞利浦RC6协议，支持模式位 | 20位数据 |
| **Sony** | 4 | 索尼协议，支持12/15/20位格式 | 12/15/20位数据（自动识别） |
| **Samsung** | 5 | 三星协议，类似NEC但引导码不同 | 32位数据（地址+命令） |

**协议解析特性**：
- **使用IRremoteESP8266库**：直接使用成熟的IRremoteESP8266库进行协议解析，解析稳定性和准确率更高
- 自动识别协议类型和数据格式（NEC、RC5、RC6、Sony、Samsung等）
- 支持重复码检测（NEC协议）
- 未匹配协议自动使用时序特征哈希值作为标识
- **解析稳定性**：相同按键每次接收都能得到相同的协议数据值，解决了哈希值不一致的问题

**时序生成特性**：
- 所有协议信号统一转换为时序数组格式
- 精确的时序控制，确保信号发送准确性
- 支持微秒级精度控制

## 三、功能使用

### LED 状态指示
- **慢闪（500ms）**：AP 模式
- **快闪（200ms）**：WiFi 连接中或 MQTT 连接失败
- **关闭**：WiFi 已连接（待机状态）
- **闪烁一次**：接收到红外信号
- **长亮**：正在发射红外信号
- **异常快闪**：系统错误

### 红外转发
1. 配置映射规则（A 信号 → B 信号）
2. 使用遥控器发送 A 信号，设备自动转发 B 信号
3. 或通过 API 触发转发

### 重置配置
当需要清除所有配置（WiFi和MQTT）并重新配置时，可以使用以下方法：

**方法1：长按按钮重置（推荐）**
- 长按设备上的按钮（GPIO4）**5秒**
- LED会快速闪烁10次提示检测到长按
- 设备会自动清除所有配置并重启
- 重启后进入AP模式，可重新配置

**方法2：Web界面配置**
- 如果Web服务器可用（AP模式或MQTT未连接），访问 `http://设备IP/` 修改配置

**方法3：串口AT指令**
- 使用串口发送AT指令修改配置

## 四、API 接口

设备提供 **HTTP REST API** 和 **MQTT** 两种接口，功能完全一致，使用统一的数据结构格式。

### 4.1 统一数据结构格式

所有接口（HTTP和MQTT）统一使用以下响应格式：

```json
{
  "success": true/false,           // 操作是否成功
  "status": "success|error|executing|ready|learning",  // 状态（HA依赖）
  "data": {...},                  // 成功时的数据（对象/数组/字符串）
  "error": "...",                 // 失败时的错误信息
  "code": 0,                      // 错误代码（可选）
  "timestamp": 1234567890         // 时间戳（毫秒）
}
```

**字段说明**：
- `success`: 布尔值，表示操作是否成功
- `status`: 字符串，表示状态
  - `"success"`: 操作成功
  - `"error"`: 操作失败
  - `"executing"`: 操作执行中（如发送信号）
  - `"ready"`: 设备就绪状态
  - `"learning"`: 设备正在学习
- `data`: 成功时的数据，可以是：
  - 对象（Object）：如状态信息、学习结果等
  - 数组（Array）：如映射规则列表、日志列表等
  - 字符串（String）：如简单的消息提示
- `error`: 失败时的错误信息（仅在`success=false`时出现）
- `code`: 错误代码（可选，仅在错误时出现）
- `timestamp`: 时间戳（毫秒）

**响应示例**：

成功响应（数据为对象）：
```json
{
  "success": true,
  "status": "success",
  "data": {
    "isLearning": true,
    "progress": "2/5",
    "currentCommand": "温度+"
  },
  "timestamp": 1234567890
}
```

成功响应（数据为数组）：
```json
{
  "success": true,
  "status": "success",
  "data": [
    {
      "id": 1,
      "aSignal": {...},
      "bSequence": {...}
    }
  ],
  "timestamp": 1234567890
}
```

成功响应（数据为字符串）：
```json
{
  "success": true,
  "status": "success",
  "data": "学习已启动",
  "timestamp": 1234567890
}
```

错误响应：
```json
{
  "success": false,
  "status": "error",
  "error": "Invalid JSON",
  "code": 400,
  "timestamp": 1234567890
}
```

状态响应（MQTT state主题，设备状态信息）：
```json
{
  "success": true,
  "status": "ready",
  "data": {
    "forwarding_enabled": true,
    "learn_mode": "none",
    "learn_device": "",
    "learn_progress": "",
    "rule_count": 5
  },
  "timestamp": 1234567890
}
```

### 4.2 接口映射关系

设备提供两种接口：**HTTP REST API** 和 **MQTT**，功能完全一致。MQTT 的 `state` 主题响应做了结构增强：`data` 始终为对象且包含 `forwarding_enabled`；当接口本身返回的 `data` 不是对象（如数组/字符串）时，返回值放在 `data.result`。

| 功能 | HTTP REST API | MQTT Action | 说明 |
|------|---------------|-------------|------|
| 获取设备状态 | GET `/api/status` | `get_status` | 返回设备状态信息 |
| 查询映射规则 | GET `/api/maps` | `query_maps` | 返回所有规则数组 |
| 更新映射规则 | PUT `/api/maps` | `update_map` | 支持单个和批量更新 |
| 获取日志 | GET `/api/logs` | `get_logs` | 返回所有日志数组（系统只保留最近3条） |
| 启动学习 | POST `/api/learn/start` | `start_learn` | 启动设备学习模式 |
| 查询学习状态 | GET `/api/learn/status` | `query_learn_status` | 返回学习进度（简化版：isLearning, progress, currentCommand） |
| 停止学习 | POST `/api/learn/stop` | `stop_learn` | 停止设备学习 |
| 查询学习结果 | GET `/api/learn/result` | `query_learn_result` | 返回学习结果（支持format参数：string/json，默认string） |
| 获取最近信号 | GET `/api/signals/recent` | `get_recent_signals` | 返回信号数组 |
| 发送信号 | POST `/api/signals/send` | `send_signal` | 发送红外信号（支持JSON和字符串格式） |
| 设置转发状态 | - | `set_forwarding_enabled` | MQTT 特有 |

### 4.3 HTTP REST API

**可用条件**：HTTP API 仅在以下情况可用：
- AP 模式（立即启动）
- WiFi 已连接但 MQTT 未配置
- WiFi 已连接但 MQTT 连接失败
- WiFi 已连接且 MQTT 已连接，但 Home Assistant Discovery 发布失败（用于排查问题）

**优化说明**：
- 系统启动后会有8秒延迟判断期，如果MQTT在此期间连接成功且Discovery发布成功，则不会启动Web服务器，节省约5-10KB内存
- MQTT 正常连接且 Discovery 成功发布时，Web 服务器会自动停止以节省资源

#### 接口列表

| 接口 | 方法 | 说明 |
|------|------|------|
| `/api/status` | GET | 获取设备状态 |
| `/api/maps` | GET | 查询所有映射规则 |
| `/api/maps` | PUT | 更新映射规则（支持单个和批量更新） |
| `/api/logs` | GET | 获取日志 |
| `/api/learn/start` | POST | 启动设备学习模式 |
| `/api/learn/status` | GET | 查询学习状态和进度 |
| `/api/learn/stop` | POST | 停止设备学习模式 |
| `/api/learn/result` | GET | 查询学习结果（支持format参数：string/json，默认string） |
| `/api/signals/recent` | GET | 获取最近接收的信号 |
| `/api/signals/send` | POST | 直接发送红外信号 |

#### 请求示例

**启动设备学习：**
```bash
POST http://设备IP/api/learn/start
Content-Type: application/json

{
  "deviceName": "空调遥控器",
  "commandNames": "开机,关机,温度+,温度-,模式",
  "timeoutSeconds": 30
}
```

**响应示例：**
```json
{
  "success": true,
  "status": "success",
  "data": "学习已启动",
  "timestamp": 1234567890
}
```

**查询学习状态：**
```bash
GET http://设备IP/api/learn/status
```

**响应示例：**
```json
{
  "success": true,
  "status": "success",
  "data": {
    "isLearning": true,
    "progress": "2/5",
    "currentCommand": "温度+"
  },
  "timestamp": 1234567890
}
```

**说明：**
- `isLearning`：是否正在学习中
- `progress`：学习进度，格式为"已学习数/总数"
- `currentCommand`：当前正在学习的指令名称

**学习流程：**
1. 调用`/api/learn/start`启动学习
2. 轮询`/api/learn/status`查询状态
3. 当`progress`变化时，表示当前信号学习成功，可以继续下一个信号
4. 当学习完成时，调用`/api/learn/result`获取学习结果

**重复信号检测：**
- 学习过程中会自动检测重复信号
- 如果连续接收到相同的信号，会在日志中记录并显示时间间隔
- 日志格式：`检测到重复信号: [信号描述]，间隔: [间隔]ms`
- 重复信号仍会被保存（不去重），便于用户了解信号接收情况
- 可通过串口输出或日志API查看重复信号信息

**停止设备学习：**
```bash
POST http://设备IP/api/learn/stop
```

**查询学习结果：**
```bash
GET http://设备IP/api/learn/result
GET http://设备IP/api/learn/result?format=string    # 字符串序列格式（默认）
GET http://设备IP/api/learn/result?format=json      # JSON格式（信号序列JSON对象）
```

**响应示例**（字符串格式，默认）：
```json
{
  "success": true,
  "status": "success",
  "data": {
    "manufacturer": "Learned",
    "supportedModels": ["Learned Device"],
    "commandsEncoding": "Raw",
    "supportedController": "MQTT",
    "commands": {
      "开机": "ABC123==@30;DEF456==|2",
      "关机": "[8991,-4546]@50;[1000,-2000]|1",
      "温度+": "1,0xFF00FF00@30;1,0x87654321|1"
    }
  },
  "timestamp": 1234567890
}
```

**说明**：
- `commands`字段中每个命令的值是字符串序列格式
- 示例说明：
  - `"ABC123==@30;DEF456==|2"`：Base64信号序列，两个信号间隔30ms，序列重复2次
  - `"[8991,-4546]@50;[1000,-2000]|1"`：数组信号序列，两个信号间隔50ms
  - `"1,0xFF00FF00@30;1,0x87654321|1"`：协议信号序列（NEC协议），两个信号间隔30ms

**响应示例**（JSON格式，format=json）：
```json
{
  "success": true,
  "status": "success",
  "data": {
    "manufacturer": "Learned",
    "supportedModels": ["Learned Device"],
    "commandsEncoding": "Raw",
    "supportedController": "MQTT",
    "commands": {
      "开机": {
        "signals": [
          {
            "protocol": 0,
            "data": "0x00000000",
            "useTiming": true,
            "timingBase64": "ABC123XYZ456...",
            "timingCount": 156,
            "repeatCount": 1
          },
          {
            "protocol": 1,
            "data": "0x00003039",
            "useTiming": false,
            "timingBase64": "",
            "timingCount": 0,
            "repeatCount": 1
          }
        ],
        "intervals": [0, 30]
      },
      "关机": {
        "signals": [
          {
            "protocol": 1,
            "data": "0x00003039",
            "useTiming": false,
            "timingBase64": "",
            "timingCount": 0,
            "repeatCount": 1
          }
        ],
        "intervals": []
      }
    }
  },
  "timestamp": 1234567890
}
```

**说明**：
- `commands`字段中每个命令的值是信号序列JSON对象
- 序列对象包含：
  - `signals`：信号数组，每个信号包含`protocol`、`data`、`useTiming`、`timingBase64`、`timingCount`、`repeatCount`字段
  - `intervals`：间隔数组（毫秒）
- 示例说明：
  - `"开机"`：包含2个信号的序列，间隔30ms
  - `"关机"`：单个信号序列（`signalCount=1`，`intervals=[]`）

**格式说明：**
- **字符串格式**（`format=string`或默认）：返回JSON格式，`data`字段包含SmartIR格式的JSON对象，`commands`字段的值是字符串序列格式
  - 信号序列格式详见2.3节
  - `commands`字段中每个命令的值是字符串序列格式（如`"ABC123==@30;DEF456==|2"`），可直接用于API发送信号
  - 支持单个信号和信号序列
- **JSON格式**（`format=json`）：返回JSON格式，`data`字段包含JSON对象，`commands`字段的值是信号序列JSON对象
  - `commands`字段中每个命令的值是信号序列JSON对象（包含`signals`和`intervals`字段）
  - 与SmartIR不兼容，但提供完整的信号序列信息
  - 支持单个信号和信号序列

**重要**：
- **默认返回字符串格式**，便于直接使用和发送
- 学习、映射、发送使用统一的Sequence格式，确保格式一致性
- 字符串格式支持序列，可直接用于API发送
- JSON格式可直接复制到SmartIR JSON配置文件使用
- SmartIR发送MQTT消息时，会直接发送`commands`字段中的值（字符串），设备会自动解析这些格式，支持序列格式
- HTTP API 支持 JSON 与字符串；MQTT 的 command 主题仅接受 JSON（须含 `action`），字符串格式须发往 SmartIR 主题

**获取最近接收的信号：**
```bash
GET http://设备IP/api/signals/recent
```

**响应示例**：
```json
[
  {
    "signals": [
      {
        "protocol": 1,
        "data": "0x00003039",
        "useTiming": false,
        "timingBase64": "",
        "timingCount": 0,
        "repeatCount": 1
      }
    ],
    "intervals": [],
    "timestamp": 1234567890,
    "matched": true
  }
]
```

**说明**：
- 返回格式为信号序列数组，每个序列包含：
  - `signals`：信号数组（单个信号也是序列，signalCount=1）
  - `intervals`：间隔数组（可能是 signalCount 或 signalCount-1 个间隔，详见信号序列说明）
  - `timestamp`：时间戳（仅用于最近接收）
  - `matched`：是否在映射库中匹配（仅用于最近接收，仅第一个信号）
- **注意**：序列中信号的`repeatCount`字段控制重复次数（使用序列中第一个信号的`repeatCount`字段）
- 单个信号也是序列（`signalCount=1`，`intervals=[]` 或 `intervals=[delay]`）
- 序列检测逻辑：间隔 < 1000ms 视为同一序列，间隔 > 5000ms 视为新序列开始
- 最多保留最近3条记录
- 注意：`isProtocol` 字段已移除，可通过 `!useTiming` 推导

**发送红外信号：**

HTTP API和MQTT接口使用完全统一的格式，都直接接收字符串格式（不进行JSON包装）。

**格式1：Base64字符串**（纯字符串，Content-Type: text/plain）
```bash
POST http://设备IP/api/signals/send
Content-Type: text/plain

Base64字符串|repeatCount
```
- 格式：`Base64字符串|repeatCount`（`|repeatCount`可选，默认为1）
- 示例：`"ABC123==|2"` 表示重复2次，`"ABC123=="` 表示重复1次

**格式2：数组字符串**（纯字符串，Content-Type: text/plain）
```bash
POST http://设备IP/api/signals/send
Content-Type: text/plain

[8991,-4546,553,-578,...]|repeatCount
```
- 格式：`[数组]|repeatCount`（`|repeatCount`可选，默认为1）
- 示例：`"[8991,-4546]|3"` 表示重复3次，`"[8991,-4546]"` 表示重复1次

**格式3：协议格式字符串**（纯字符串，Content-Type: text/plain）
```bash
POST http://设备IP/api/signals/send
Content-Type: text/plain

1,0xFF00FF00,1
```
- **2参数格式**（向后兼容）：`protocol,data`（repeatCount默认为1）
- **3参数格式**（向后兼容）：`protocol,data,repeatCount`
  - protocol：协议编号（1=NEC, 2=RC5, 3=RC6, 4=Sony, 5=Samsung）
  - data：协议数据（十六进制格式，如0xFF00FF00）
  - repeatCount：重复次数（默认1）
- 示例：
  - 标准协议：`"1,0xFF00FF00,2"`（NEC协议，重复2次）

**格式4：JSON序列格式**（统一信号格式，Content-Type: application/json）
```bash
POST http://设备IP/api/signals/send
Content-Type: application/json

{
  "signals": [
    {
      "protocol": 1,
      "data": "0x00003039",
      "useTiming": false,
      "timingBase64": "",
      "timingCount": 0,
      "repeatCount": 1
    }
  ],
  "intervals": [0, 30]
}
```

**说明**：
- **支持两种格式**：JSON格式和字符串格式
- **JSON格式**：Content-Type为`application/json`时，使用序列格式JSON（`{"signals":[...], "intervals":[...]}`）
- **字符串格式**：Content-Type为`text/plain`或未指定时，使用字符串序列格式（详见2.3节）
- **字符串格式优势**：简洁易读，可直接使用学习结果，支持序列格式
- **JSON格式优势**：结构化数据，便于程序处理
- 所有格式最终都转换为SignalSequence格式发送
- 序列中信号的`repeatCount`字段控制重复次数（不再使用外部`repeatCount`参数）

**更新映射规则：**

`PUT /api/maps` 接口支持两种更新方式：

**方式1：单个更新**（使用 `id` 和 `rule`）
```bash
PUT http://设备IP/api/maps
Content-Type: application/json

{
  "id": 1,
  "rule": {
    "aSignal": {
      "protocol": 1,
      "data": "0x00003039",
      "useTiming": false,
      "timingBase64": "",
      "timingCount": 0,
      "repeatCount": 1
    },
    "bSequence": {
      "signals": [{
        "protocol": 1,
        "data": "0x00010932",
        "useTiming": false,
        "timingBase64": "",
        "timingCount": 0,
        "repeatCount": 1
      }],
      "intervals": []
    }
  }
}
```

**方式2：批量更新**（使用 `rules` 数组）
```bash
PUT http://设备IP/api/maps
Content-Type: application/json

{
  "rules": [
    {
      "aSignal": {...},
      "bSequence": {...}
    },
    ...
  ]
}
```

**说明**：
- 单个更新：更新指定ID的映射规则
- 批量更新：清空现有规则，添加新规则列表
- 接口会自动识别是单个更新还是批量更新（检查是否包含 `rules` 数组）

### MQTT 接口（可选）

**主题：**
- 指令：`homeassistant/orvibo-ct30w/command`（订阅此主题接收指令）
- 状态：`homeassistant/orvibo-ct30w/state`（设备发布状态信息）
- 日志：`homeassistant/orvibo-ct30w/log`（设备发布日志信息）

**state 主题响应结构：**
- `data` 始终为对象，且必含 `forwarding_enabled`
- 若接口原始返回不是对象（例如数组/字符串），则放入 `data.result`

**SmartIR 支持：**
设备自动订阅 SmartIR 主题，支持SmartIR发送的所有格式：
- `smartir/climate/+/send` - 气候设备（空调、暖气等）
- `smartir/media_player/+/send` - 媒体播放器（电视、音响等）
- `smartir/fan/+/send` - 风扇设备
- `smartir/light/+/send` - 灯光设备

**支持的格式**：
- **字符串序列格式**（详见2.3节）：
  - 单个信号：`ABC123==|2`、`[8991,-4546]|3`、`1,0xFF00FF00,2`
  - 序列格式：`ABC123==@30;DEF456==|2`
  - 第一个信号前间隔：`@50;ABC123==@30;DEF456==|1`
- **JSON格式**：序列格式JSON（`{"signals":[...], "intervals":[...]}`）

SmartIR发送的payload（就是JSON配置文件中commands字段的值）会被设备自动解析和处理，支持序列格式，无需任何转换。

**支持的指令（action）：**

| action | 说明 | 必需参数 | 对应 HTTP API |
|--------|------|----------|---------------|
| `query_maps` | 查询所有映射规则 | 无 | GET `/api/maps` |
| `update_map` | 更新映射规则（支持单个和批量） | `id`, `rule` 或 `rules` | PUT `/api/maps` |
| `get_logs` | 获取日志 | 无 | GET `/api/logs` |
| `start_learn` | 启动设备学习 | `deviceName`, `commandNames`, `timeoutSeconds` | POST `/api/learn/start` |
| `query_learn_status` | 查询学习状态 | 无 | GET `/api/learn/status` |
| `stop_learn` | 停止设备学习 | 无 | POST `/api/learn/stop` |
| `query_learn_result` | 查询学习结果 | 无 | GET `/api/learn/result` |
| `get_recent_signals` | 获取最近接收的信号 | 无 | GET `/api/signals/recent` |
| `send_signal` | 发送红外信号 | JSON（须含 `action`、`signal` 或 `signals`）；直接字符串仅限 SmartIR 主题 | POST `/api/signals/send` |
| `set_forwarding_enabled` | 设置转发启用状态 | `enabled` | - |
| `get_status` | 获取设备状态 | 无 | GET `/api/status` |

**指令示例：**

**1. 查询所有映射规则：**
```json
{
  "action": "query_maps"
}
```

**响应**（发布到 state 主题）：
```json
{
  "success": true,
  "status": "success",
  "data": {
    "forwarding_enabled": true,
    "result": [{"id":1,"aSignal":{...},"bSequence":{...},...}]
  },
  "timestamp": 1234567890
}
```

**2. 更新映射规则（单个更新）：**
```json
{
  "action": "update_map",
  "id": 1,
  "rule": {
    "aSignal": {
      "protocol": 1,
      "data": "0x00003039",
      "useTiming": false,
      "timingBase64": "",
      "timingCount": 0,
      "repeatCount": 1
    },
    "bSequence": {
      "signals": [{
        "protocol": 1,
        "data": "0x00010932",
        "useTiming": false,
        "timingBase64": "",
        "timingCount": 0,
        "repeatCount": 1
      }],
      "intervals": []
    }
  }
}
```

**3. 批量更新映射规则（使用 `update_map`，包含 `rules` 数组）：**
```json
{
  "action": "update_map",
  "rules": [
    {
      "aSignal": {
        "protocol": 1,
        "data": "0x00003039",
        "useTiming": false,
        "timingBase64": "",
        "timingCount": 0,
        "repeatCount": 1
      },
      "bSequence": {
        "signals": [{
          "protocol": 1,
          "data": "0x00010932",
          "useTiming": false,
          "timingBase64": "",
          "timingCount": 0,
          "repeatCount": 1
        }],
        "intervals": []
      }
    }
  ]
}
```

**说明**：`update_map` 接口同时支持单个更新（使用 `id` 和 `rule`）和批量更新（使用 `rules` 数组），与 HTTP API 保持一致。

**4. 获取日志：**
```json
{
  "action": "get_logs"
}
```

**5. 启动设备学习：**
```json
{
  "action": "start_learn",
  "deviceName": "空调遥控器",
  "commandNames": "开机,关机,温度+,温度-,模式",
  "timeoutSeconds": 30
}
```

**响应**（发布到 state 主题）：
```json
{
  "success": true,
  "status": "success",
  "data": {
    "forwarding_enabled": true,
    "result": "学习已启动"
  },
  "timestamp": 1234567890
}
```

**6. 查询学习状态：**
```json
{
  "action": "query_learn_status"
}
```

**响应**（发布到 state 主题）：
```json
{
  "success": true,
  "status": "success",
  "data": {
    "forwarding_enabled": true,
    "isLearning": true,
    "progress": "2/5",
    "currentCommand": "温度+"
  },
  "timestamp": 1234567890
}
```

**说明**：响应格式与HTTP API一致，学习流程详见 [HTTP REST API - 学习流程](#学习流程)。

**7. 停止设备学习：**
```json
{
  "action": "stop_learn"
}
```

**8. 查询学习结果：**
```json
{
  "action": "query_learn_result",
  "format": "string"
}
```

**format参数**（可选，默认string）：
- `string`：字符串序列格式（默认），返回JSON格式，`data`字段包含SmartIR格式的JSON对象，`commands`字段的值是字符串序列格式
- `json`：JSON格式，返回JSON格式，`data`字段包含JSON对象，`commands`字段的值是信号序列JSON对象

**响应示例**（字符串格式，默认）：
```json
{
  "success": true,
  "status": "success",
  "data": {
    "forwarding_enabled": true,
    "manufacturer": "Learned",
    "supportedModels": ["Learned Device"],
    "commandsEncoding": "Raw",
    "supportedController": "MQTT",
    "commands": {
      "开机": "ABC123==@30;DEF456==|2",
      "关机": "[8991,-4546]@50;[1000,-2000]|1",
      "音量+": "1,0xFF00FF00@30;1,0x87654321|1"
    }
  },
  "timestamp": 1234567890
}
```

**响应示例**（JSON格式，format=json）：
```json
{
  "success": true,
  "status": "success",
  "data": {
    "forwarding_enabled": true,
    "manufacturer": "Learned",
    "supportedModels": ["Learned Device"],
    "commandsEncoding": "Raw",
    "supportedController": "MQTT",
    "commands": {
      "开机": {
        "signals": [
          {
            "protocol": 0,
            "data": "0x00000000",
            "useTiming": true,
            "timingBase64": "ABC123XYZ456...",
            "timingCount": 156,
            "repeatCount": 1
          },
          {
            "protocol": 1,
            "data": "0x00003039",
            "useTiming": false,
            "timingBase64": "",
            "timingCount": 0,
            "repeatCount": 1
          }
        ],
        "intervals": [0, 30]
      },
      "关机": {
        "signals": [
          {
            "protocol": 1,
            "data": "0x00003039",
            "useTiming": false,
            "timingBase64": "",
            "timingCount": 0,
            "repeatCount": 1
          }
        ],
        "intervals": []
      }
    }
  },
  "timestamp": 1234567890
}
```

**说明**：
- `commands`字段中每个命令的值是信号序列JSON对象
- 序列对象包含：
  - `signals`：信号数组，每个信号包含`protocol`、`data`、`useTiming`、`timingBase64`、`timingCount`、`repeatCount`字段
  - `intervals`：间隔数组（毫秒）
- 示例说明：
  - `"开机"`：包含2个信号的序列，间隔30ms
  - `"关机"`：单个信号序列（`signalCount=1`，`intervals=[]`）

**9. 获取最近接收的信号：**
```json
{
  "action": "get_recent_signals"
}
```

**10. 发送红外信号：**

**方式1：command 主题** — 必须为合法 JSON，且带 `action: "send_signal"`。直接发 `1,0xFD18E7`、`ABC123==` 等纯字符串会报 `Invalid JSON`，不支持。主题：`homeassistant/orvibo-ct30w/command`。

单信号示例：
```json
{"action": "send_signal", "signal": {"protocol": 1, "data": "0x00003039", "useTiming": false}, "repeatCount": 1}
```
序列示例：
```json
{"action": "send_signal", "signals": [{"protocol": 1, "data": "0x00003039", "useTiming": false, "repeatCount": 1}], "intervals": [0, 30]}
```

**方式2：SmartIR 主题** — 支持直接字符串（协议、Base64、数组、序列，格式见 2.3 节）。主题：`smartir/climate/设备ID/send`、`smartir/media_player/设备ID/send` 等。Payload 示例：`1,0xFF00FF00,2`、`ABC123==|2`、`[8991,-4546]|3`、`ABC123==@30;DEF456==|2`。

**11. 设置转发启用状态：**
```json
{
  "action": "set_forwarding_enabled",
  "enabled": true
}
```

**响应格式：**
所有MQTT接口使用统一的响应格式，详见 [4.1 统一数据结构格式](#41-统一数据结构格式)。

**状态响应**（MQTT state主题）：
所有指令执行后，设备会在 `state` 主题发布统一格式的响应。设备状态发布采用**完全事件驱动**机制：

- **事件驱动更新**：设备状态只在以下情况实际变化时发布：
  - 学习开始/停止（`learn_mode`、`status` 变化）
  - 转发开关变化（`forwarding_enabled` 变化）
  - 映射规则数量变化（`rule_count` 变化）
  - Discovery 发布成功后首次连接
- **查询响应独立**：查询接口（如 `query_maps`、`get_logs` 等）的响应发布到 `state` 主题，不会被设备状态发布覆盖
- **状态信息格式**：状态发布时，设备状态信息在`data`字段中：
```json
{
  "success": true,
  "status": "ready",  // 或 "learning", "error"
  "data": {
    "forwarding_enabled": true,
    "learn_mode": "none",
    "learn_device": "",
    "learn_progress": "",
    "rule_count": 5
  },
  "timestamp": 1234567890
}
```

### Home Assistant 集成

设备支持 Home Assistant 的 MQTT Discovery 自动发现功能。

#### 自动发现

设备连接 MQTT 后会自动发布 Discovery 配置，Home Assistant 会自动发现并创建设备实体：

- **二进制传感器**：设备在线状态（`orvibo_ct30w_status`）
- **传感器**：设备状态信息（`orvibo_ct30w_state`），包含 JSON 属性
  - 主状态：`ready` / `learning` / `error`
  - 属性：
    - `forwarding_enabled`：转发是否启用（布尔值）
    - `learn_mode`：当前学习模式（`none` / `device`）
    - `learn_device`：当前学习的设备名称
    - `learn_progress`：学习进度（如"2/5"）
    - `rule_count`：映射规则数量
    - ~~`recent_signal`~~：已移除（为提升转发性能，不再自动更新）。如需最近信号信息，请使用 `get_recent_signals` API接口查询
- **开关**：启用转发（`orvibo_ct30w_forwarding_enabled`）
  - 状态：转发功能的启用状态（`true` / `false`）
  - 可通过界面直接操作，或通过 MQTT 命令控制
- **文本传感器**：日志信息（`orvibo_ct30w_log`）
  - 状态：最新日志条目（格式：`[类型] 内容`）
  - 事件驱动更新：每次有新日志时自动推送最新一条到MQTT主题，Home Assistant会自动记录日志变更历史
  - 如果无日志，显示"无日志"

#### 使用说明

1. **确保 MQTT 集成已启用**：在 Home Assistant 中，进入 **设置 > 设备与集成 > MQTT**，确认已启用自动发现
2. **设备连接**：设备连接 MQTT 后会自动发布 Discovery 配置
3. **查看设备**：在 Home Assistant 的 **设置 > 设备与集成** 中可以看到 "ORVIBO CT30W 红外转发器" 设备

#### 故障排除

如果设备未在 Home Assistant 中显示：

1. **检查串口输出**：查看是否有 `[HA Discovery]` 相关的日志
2. **检查 MQTT 连接**：确认设备已成功连接到 MQTT 服务器
3. **检查缓冲区大小**：如果 Discovery 发布失败，可能是 MQTT 缓冲区不足（已设置为 2048 字节）
4. **手动触发重新发现**：在 Home Assistant 中，进入 MQTT 集成，点击 **重新配置** 或 **重新加载**
5. **使用 Web 接口排查**：如果 Discovery 发布失败，Web 服务器会自动启用，可通过 `http://设备IP` 访问进行排查

#### Discovery 失败时的处理

如果 Discovery 发布失败：
- Web 服务器会自动启用，可通过 Web 接口访问设备
- 设备会使用递增重试间隔自动重试发布 Discovery 配置：
  - 第1次重试：连接后1秒，之后每5秒重试
  - 第2次重试：每10秒重试
  - 第3次及以后：每20秒重试
- 可通过串口监视器查看详细的错误信息

#### 在 Home Assistant 中调用接口

设备通过 MQTT 提供服务调用接口。在 Home Assistant 中，可以使用 `mqtt.publish` 服务来调用这些接口。

**1. 查询学习结果：**
```yaml
service: mqtt.publish
data:
  topic: homeassistant/orvibo-ct30w/command
  payload: |
    {
      "action": "query_learn_result",
      "format": "string"
    }
```

响应会在 `homeassistant/orvibo-ct30w/state` 主题中发布。默认返回字符串序列格式，每行一个命令。

**2. 获取设备状态：**
```yaml
service: mqtt.publish
data:
  topic: homeassistant/orvibo-ct30w/command
  payload: |
    {
      "action": "get_status"
    }
```

响应会在 `homeassistant/orvibo-ct30w/state` 主题中发布，格式与设备状态发布一致。

**3. 发送红外信号：**

**`homeassistant/orvibo-ct30w/command` 主题仅接受合法 JSON**（或 `true`/`false` 用于转发开关）。直接发送 `1,0xFD18E7` 等纯字符串会报 `Invalid JSON`。

方式一：`action: send_signal` + 单个 `signal`
```yaml
service: mqtt.publish
data:
  topic: homeassistant/orvibo-ct30w/command
  payload: |
    {
      "action": "send_signal",
      "signal": {
        "protocol": 1,
        "data": "0x00003039",
        "useTiming": false
      },
      "repeatCount": 1
    }
```

方式二：`action: send_signal` + 序列 `signals`
```yaml
service: mqtt.publish
data:
  topic: homeassistant/orvibo-ct30w/command
  payload: |
    {
      "action": "send_signal",
      "signals": [
        { "protocol": 1, "data": "0x00003039", "useTiming": false, "repeatCount": 1 },
        { "protocol": 1, "data": "0x0000303A", "useTiming": false, "repeatCount": 1 }
      ],
      "intervals": [0, 30]
    }
```

方式三：**直接字符串**（协议、Base64、数组、序列等）须发到 **SmartIR 主题**，不能发到 `command`。主题示例：`smartir/media_player/你的设备ID/send`，`你的设备ID` 改为 SmartIR 配置中的 ID。
```yaml
service: mqtt.publish
data:
  topic: smartir/media_player/your_tv/send
  payload: "1,0xFF00FF00,2"
```
其它字符串示例：`ABC123==|2`、`[8991,-4546]|3`、`ABC123==@30;DEF456==|2`，格式见 2.3 节。

**4. 控制转发开关：**
转发开关实体可以直接在 Home Assistant 界面中操作，也可以通过服务调用：

方式一：直接发送 `true` 或 `false`（推荐，用于开关实体）：
```yaml
service: mqtt.publish
data:
  topic: homeassistant/orvibo-ct30w/command
  payload: "true"  # 或 "false" 来禁用
```

方式二：使用 JSON 格式：
```yaml
service: mqtt.publish
data:
  topic: homeassistant/orvibo-ct30w/command
  payload: |
    {
      "action": "set_forwarding_enabled",
      "enabled": true
    }
```

**4. 查询最近接收的信号：**
```yaml
service: mqtt.publish
data:
  topic: homeassistant/orvibo-ct30w/command
  payload: |
    {
      "action": "get_recent_signals"
    }
```

**5. 自动化示例（基于最近接收信号触发）：**
```yaml
automation:
  - alias: "红外信号触发"
    trigger:
      - platform: mqtt
        topic: homeassistant/orvibo-ct30w/state
    # 注意：recent_signal 属性已移除，需要通过API查询
    # 如果需要基于信号触发，建议使用日志传感器或直接查询API
    action:
      - service: mqtt.publish
        data:
          topic: homeassistant/orvibo-ct30w/command
          payload: |
            {
              "action": "get_recent_signals"
            }
      # 然后处理返回的信号数据
```

#### 统一信号格式

设备使用统一的信号结构表示红外信号：
- **单个信号**：使用 `UnifiedSignal` 结构，详见 [2.1 单个信号（UnifiedSignal）](#21-单个信号unifiedsignal)
- **信号序列**：使用 `SignalSequence` 结构，详见 [2.2 信号序列（SignalSequence）](#22-信号序列signalsequence）

**重要说明**：
- 单个信号也是序列（`signalCount=1`）
- 序列中信号的`repeatCount`字段控制重复次数（使用序列中第一个信号的`repeatCount`字段，如果>1，重复发送整个序列）
- 此格式在所有场景中统一使用：日志记录、转发规则、API接口、MQTT消息、Flash存储等
- 支持协议信号和时序码信号两种格式
- 录制的信号、最近接收的信号和规则中的信号可以直接相互拷贝使用

## 五、常见问题

### 编译与烧录
- **找不到库文件**：运行 `pio pkg install`
- **内存不足**：检查 `constants.h` 中的缓冲区大小
- **找不到串口**：检查 USB 驱动和串口号
- **上传超时**：按住 BOOT 按钮，按 RESET，再松开 BOOT；或降低 `upload_speed`

### 运行问题
- **WiFi 连接失败**：检查 SSID/密码，确认路由器支持 2.4GHz
- **MQTT 连接失败**：检查服务器地址/端口，确认认证信息
- **红外接收无响应**：检查 GPIO5 引脚和接收头供电
- **红外发射无效果**：检查 GPIO14 引脚、发射管方向和供电
- **映射规则不生效**：检查规则格式和 A 信号匹配，查看串口日志

### 调试技巧

**串口信号输出格式：**

设备接收到红外信号时，会在串口输出以下格式的信息：

- **协议信号**：
  ```
  [receive] Protocol:1, Data:11B08F7, Matched:Yes, Interval:245ms
  ```
  - `Protocol`: 协议类型（1=NEC, 2=RC5, 3=RC6, 4=Sony, 5=Samsung）
  - `Data`: 协议数据（十六进制）
  - `Matched`: 是否在映射表中匹配
  - `Interval`: 与前一个信号的时间间隔（毫秒，首次信号不显示）

- **时序码信号**：
  ```
  [receive] Timing:156 points, Base64:ABC123XYZ456... (208 chars), Matched:No, Interval:312ms
  ```
  - `Timing`: 时序点数
  - `Base64`: Base64字符串的前16个字符（用于识别）+ 总长度
  - `Matched`: 是否在映射表中匹配
  - `Interval`: 与前一个信号的时间间隔（毫秒，首次信号不显示）

所有信号都会记录到`recentSignals`缓冲区（最近3个），可通过`/api/signals/recent` API查询。

```cpp
// 检查内存（方式一：直接使用ESP API）
Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());

// 检查内存（方式二：使用MemoryMonitor工具类）
#include "utils/memory_monitor.h"
MemoryMonitor::printMemoryInfo();  // 打印详细内存信息
String memInfo = MemoryMonitor::getMemoryInfoString();  // 获取内存信息字符串
Serial.println(memInfo);

// 检查内存使用是否超过阈值（默认50%）
if (MemoryMonitor::isMemoryUsageHigh()) {
    Serial.println("警告：内存使用超过50%！");
}

// 检查 LittleFS
FSInfo fs_info;
LittleFS.info(fs_info);
Serial.printf("Total: %d, Used: %d\n", fs_info.totalBytes, fs_info.usedBytes);
```

## 六、性能指标

- **转发延迟**：≤ 50ms（A信号接收后B信号发射的最大延迟，包括1ms转发前延时和处理时间）
- **转发前延时**：1ms（用于AB信号隔离，避免信号冲突）
- **重复码识别间隔**：100-120ms（NEC协议标准，用于识别重复码信号）
- **间隔误差**：≤ 20ms（连续信号间隔的最大误差）
- **信号缓存**：最多 3 个（支持并发接收的短时间多信号）
- **内存使用**：≤ 90%（运行时内存占用控制，超过90%时自动清理）
- **协议解析准确率**：> 99%（使用IRremoteESP8266库，解析稳定可靠）
- **时序精度**：微秒级（红外信号时序控制精度）

### 性能优化实现

#### 内存优化
- **静态内存分配**：红外接收缓冲区使用静态数组，避免动态分配带来的内存碎片
- **栈溢出风险修复**：时序缓冲区从静态数组改为类成员变量，避免栈溢出（ESP8266栈空间约4KB）
- **缓冲区大小优化**：时序数组从500减小到300，节省约1.6KB内存
- **内存监控工具**：提供`MemoryMonitor`类实时监控内存使用情况
- **内存检查加强**：在JSON解析和动态分配前更严格地检查可用内存，添加安全缓冲
- **移除OTA功能**：完全移除OTA功能，释放约5-10KB内存（包括ESP8266HTTPUpdate库的静态内存占用）
- **JSON文档大小优化**：减少API响应中的JSON文档大小，降低动态内存分配峰值
- **Web服务器延迟启动**：启动后8秒延迟判断，MQTT成功则不启动Web服务器，节省5-10KB内存
- **智能内存清理**：每30秒检查一次（`MEMORY_CHECK_INTERVAL = 30000`），超过85%且连续两次检查都超过阈值时才清理，避免频繁清理
- **日志保存优化**：只在日志数量达到阈值（约13条）或错误日志时才保存，减少Flash写入

#### 转发延迟优化
- **异步消息队列**：使用消息队列解耦接收和转发流程，避免阻塞
- **快速返回机制**：转发过程中快速检查并返回，减少不必要的等待
- **队列处理优化**：队列处理间隔从10ms优化到5ms，提升响应速度
- **红外发射任务优化**：发射任务间隔优化到5ms，提升响应速度
- **主循环优化**：移除主循环中的`delay(1)`阻塞，改用非阻塞`yield()`，提升实时性
- **非阻塞延时机制**：使用`PreciseTimer`实现非阻塞延时，转发前延时1ms用于AB信号隔离，不阻塞主循环
- **延迟时间优化**：
  - 转发前延时：优化到1ms（用于AB信号隔离，避免信号冲突）
  - 最大延迟约束：50ms（包括转发前延时和处理时间）
  - 重复码识别：100-120ms间隔（NEC协议标准，用于识别重复码信号）
- **IRsend库集成**：NEC协议使用IRremoteESP8266库的IRsend直接发送，时序更准确，性能更好
- **信号记录优化**：所有信号都输出到串口和recentSignals缓冲区，不再写入日志缓冲区和MQTT推送，大幅减少转发路径上的开销
  - 串口输出包含信号详情（协议信号显示协议和数据，时序码信号显示时序点数和Base64前缀）
  - 显示与前一个信号的时间间隔，便于调试
  - recentSignals缓冲区保留最近3个信号，可通过API查询
- **减少调试输出**：移除不必要的Serial输出，减少阻塞时间

#### 间隔误差优化
- **硬件定时器**：使用ESP8266 Ticker库实现毫秒级精准延时（误差≤20ms）
- **微秒级精度**：使用优化的`delayMicroseconds`实现微秒级时序控制
- **中断管理优化**：
  - 优化中断关闭时间，确保不超过10ms限制
  - 中断关闭检查频率优化：每1ms检查一次（每100个时序点），而不是每个时序点检查，减少开销
  - 接收器中断处理优化：减少对volatile变量的访问次数，提升性能
- **MQTT回调优化**：完全避免String对象创建，使用纯C字符串操作，减少内存分配和栈溢出风险

#### 代码架构优化
- **统一响应处理**：Web API 和 MQTT 使用统一的 `APIResponseHandler` 接口，消除约200行重复代码
- **统一错误处理**：所有接口使用统一的错误处理逻辑，包含详细的错误日志和串口调试信息
- **统一响应格式**：Web API 和 MQTT 完全使用相同的响应格式和错误消息格式
- **MQTT缓冲区优化**：MQTT缓冲区从1024字节增加到2048字节，支持更大的响应消息（如包含多个映射规则的查询结果）
- **错误检查增强**：所有MQTT发布操作都进行返回值检查，失败时输出详细的错误信息（连接状态、消息大小、内存状态等）
