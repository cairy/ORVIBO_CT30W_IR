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

## 二、信号参数说明

红外信号数据结构包含以下字段：

```json
{
  "protocol": 1,      // 协议类型：1=NEC, 2=RC5, 3=RC6, 4=Sony, 5=Samsung
  "data": 12345,      // 协议数据（标准协议解析出的数据值）
  "hash": 0,          // 时序特征哈希（无法匹配标准协议时使用）
  "isProtocol": true  // 是否为标准协议：true=标准协议，false=使用hash匹配
}
```

**说明**：
- `isProtocol=true` 时，使用 `protocol` 和 `data` 进行匹配
- `isProtocol=false` 时，使用 `hash` 进行匹配（用于非标准协议）

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

### HTTP REST API

**注意**：HTTP API 仅在以下情况可用：
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
| `/api/learn/result` | GET | 查询学习结果（SmartIR格式） |
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
  "message": "学习已启动"
}
```

**查询学习状态：**
```bash
GET http://设备IP/api/learn/status
```

**响应示例：**
```json
{
  "isLearning": true,
  "progress": "2/5",
  "currentCommand": "温度+"
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

**停止设备学习：**
```bash
POST http://设备IP/api/learn/stop
```

**查询学习结果：**
```bash
GET http://设备IP/api/learn/result
GET http://设备IP/api/learn/result?format=base64    # Base64格式（默认）
GET http://设备IP/api/learn/result?format=signalid  # SignalID格式（如果解析成功）
GET http://设备IP/api/learn/result?format=array     # 数组格式
```

**响应示例**（Base64格式，默认）：
```json
{
  "manufacturer": "Learned",
  "supportedModels": ["Learned Device"],
  "commandsEncoding": "Raw",
  "supportedController": "MQTT",
  "commands": {
    "开机": "Base64编码的时序数据",
    "关机": "Base64编码的时序数据",
    "温度+": "Base64编码的时序数据"
  }
}
```

**响应示例**（协议格式，如果解析成功）：
```json
{
  "manufacturer": "Learned",
  "supportedModels": ["Learned Device"],
  "commandsEncoding": "Raw",
  "supportedController": "MQTT",
  "commands": {
    "开机": "1,0xFF00FF00,1",
    "关机": "1,0xFF10EF,1"
  }
}
```

**响应示例**（数组格式）：
```json
{
  "manufacturer": "Learned",
  "supportedModels": ["Learned Device"],
  "commandsEncoding": "Raw",
  "supportedController": "MQTT",
  "commands": {
    "开机": "[8991,-4546,553,-578,...]",
    "关机": "[8991,-4546,553,-578,...]"
  }
}
```

**格式说明：**
- **Base64格式**（默认）：输出SmartIR JSON配置文件格式，可直接复制到SmartIR JSON配置文件使用
- **协议格式**：如果信号解析成功，输出协议格式字符串（`protocol,data,repeatCount`），protocol是数字常量（1=NEC, 2=RC5等）
- **数组格式**：输出数组格式字符串，兼容SmartIR JSON配置文件格式

**重要**：
- 学习结果输出为**完整的SmartIR JSON配置文件格式**，可以直接复制到SmartIR JSON配置文件中使用
- SmartIR发送MQTT消息时，会直接发送`commands`字段中的值（字符串），设备会自动解析这些格式
- HTTP API和MQTT接口使用完全统一的格式，都直接接收字符串格式（Base64/数组/协议）

**获取最近接收的信号：**
```bash
GET http://设备IP/api/signals/recent
```

**响应示例**：
```json
[
  {
    "signal": {
      "protocol": 1,
      "data": 12345,
      "hash": 0,
      "isProtocol": true
    },
    "timestamp": 1234567890,
    "matched": true
  }
]
```

**说明**：
- `protocol`：协议类型（1=NEC, 2=RC5, 3=RC6, 4=Sony, 5=Samsung）
- `data`：协议数据
- `hash`：时序特征哈希（非标准协议时使用）
- `isProtocol`：是否为标准协议
- `matched`：是否在映射库中匹配
- 最多保留最近10条记录

**发送红外信号：**

HTTP API和MQTT接口使用完全统一的格式，都直接接收字符串格式（不进行JSON包装）。

**格式1：Base64字符串**（纯字符串，Content-Type: text/plain）
```bash
POST http://设备IP/api/signals/send
Content-Type: text/plain

Base64字符串
```

**格式2：数组字符串**（纯字符串，Content-Type: text/plain）
```bash
POST http://设备IP/api/signals/send
Content-Type: text/plain

[8991,-4546,553,-578,...]
```

**格式3：协议格式字符串**（纯字符串，Content-Type: text/plain）
```bash
POST http://设备IP/api/signals/send
Content-Type: text/plain

1,0xFF00FF00,1
```
- 格式：`protocol,data,repeatCount`
- protocol：协议编号（1=NEC, 2=RC5, 3=RC6, 4=Sony, 5=Samsung）
- data：协议数据（十六进制格式，如0xFF00FF00）
- repeatCount：重复次数（默认1）

**格式4：SignalID对象格式**（保留现有格式，Content-Type: application/json）
```bash
POST http://设备IP/api/signals/send
Content-Type: application/json

{
  "signal": {
    "protocol": 1,
    "data": 12345,
    "hash": 0,
    "isProtocol": true
  },
  "repeatCount": 1
}
```

**说明**：
- HTTP API和MQTT接口完全统一格式
- 如果Content-Type是text/plain，按字符串格式解析（Base64/数组/协议）
- 如果Content-Type是application/json，按JSON格式解析（SignalID对象格式）
- 字符串格式可以直接使用学习结果或SmartIR JSON配置文件中的commands字段值
- SignalID对象格式保留向后兼容

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
      "data": 12345,
      "hash": 0,
      "isProtocol": true
    },
    "bSignals": [{
      "protocol": 1,
      "data": 67890,
      "hash": 0,
      "isProtocol": true
    }],
    "bSignalCount": 1,
    "intervals": [],
    "repeatCount": 2
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
      "bSignals": [...],
      "bSignalCount": 1,
      "intervals": [],
      "repeatCount": 1
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

**SmartIR 支持：**
设备自动订阅 SmartIR 主题，支持SmartIR发送的所有格式：
- `smartir/climate/+/send` - 气候设备（空调、暖气等）
- `smartir/media_player/+/send` - 媒体播放器（电视、音响等）
- `smartir/fan/+/send` - 风扇设备
- `smartir/light/+/send` - 灯光设备

**支持的格式**：
- Base64字符串：`Base64字符串`
- 数组字符串：`[8991,-4546,553,-578,...]`
- 协议格式字符串：`1,0xFF00FF00,1`（格式：`protocol,data,repeatCount`）

SmartIR发送的payload（就是JSON配置文件中commands字段的值）会被设备自动解析和处理，无需任何转换。

#### 接口映射关系

设备提供两种接口：HTTP REST API 和 MQTT，功能完全一致。以下是接口映射关系：

| HTTP REST API | MQTT Action | 说明 |
|---------------|-------------|------|
| GET `/api/status` | - | Web 特有（设备状态） |
| GET `/api/maps` | `query_maps` | 查询所有映射规则 |
| PUT `/api/maps` | `update_map` | 更新映射规则（支持单个和批量） |
| GET `/api/logs` | `get_logs` | 获取日志 |
| POST `/api/learn/start` | `start_learn` | 启动设备学习 |
| GET `/api/learn/status` | `query_learn_status` | 查询学习状态 |
| POST `/api/learn/stop` | `stop_learn` | 停止设备学习 |
| GET `/api/learn/result` | `query_learn_result` | 查询学习结果 |
| GET `/api/signals/recent` | `get_recent_signals` | 获取最近接收的信号 |
| POST `/api/signals/send` | `send_signal` | 发送红外信号 |
| - | `set_forwarding_enabled` | MQTT 特有（设置转发启用状态） |

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
| `send_signal` | 发送红外信号 | `signal`, `repeatCount` | POST `/api/signals/send` |
| `set_forwarding_enabled` | 设置转发启用状态 | `enabled` | - |

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
  "status": "success",
  "message": "[{\"id\":1,\"aSignal\":{...},\"bSignals\":[...],...}]",
  "timestamp": 1234567890
}
```

**2. 添加映射规则：**
```json
{
  "action": "add_map",
  "rule": {
    "aSignal": {
      "protocol": 1,
      "data": 12345,
      "hash": 0,
      "isProtocol": true
    },
    "bSignals": [{
      "protocol": 1,
      "data": 67890,
      "hash": 0,
      "isProtocol": true
    }],
    "bSignalCount": 1,
    "intervals": [],
    "repeatCount": 1
  }
}
```

**3. 更新映射规则（单个更新）：**
```json
{
  "action": "update_map",
  "id": 1,
  "rule": {
    "aSignal": {
      "protocol": 1,
      "data": 12345,
      "hash": 0,
      "isProtocol": true
    },
    "bSignals": [{
      "protocol": 1,
      "data": 67890,
      "hash": 0,
      "isProtocol": true
    }],
    "bSignalCount": 1,
    "intervals": [],
    "repeatCount": 2
  }
}
```

**4. 批量更新映射规则（使用 `update_map`，包含 `rules` 数组）：**
```json
{
  "action": "update_map",
  "rules": [
    {
      "aSignal": {
        "protocol": 1,
        "data": 12345,
        "hash": 0,
        "isProtocol": true
      },
      "bSignals": [{
        "protocol": 1,
        "data": 67890,
        "hash": 0,
        "isProtocol": true
      }],
      "bSignalCount": 1,
      "intervals": [],
      "repeatCount": 1
    }
  ]
}
```

**说明**：`update_map` 接口同时支持单个更新（使用 `id` 和 `rule`）和批量更新（使用 `rules` 数组），与 HTTP API 保持一致。

**5. 获取日志：**
```json
{
  "action": "get_logs"
}
```

**6. 启动设备学习：**
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
  "status": "success",
  "message": "学习已启动",
  "timestamp": 1234567890
}
```

**7. 查询学习状态：**
```json
{
  "action": "query_learn_status"
}
```

**响应**（发布到 state 主题）：
```json
{
  "status": "success",
  "message": "{\"isLearning\":true,\"deviceName\":\"空调遥控器\",\"progress\":\"2/5\",\"currentCommand\":\"温度+\",\"learnedCount\":2,\"totalCount\":5,\"learnedCommands\":[\"开机\",\"关机\"]}",
  "timestamp": 1234567890
}
```

**说明：**
- `isLearning`：是否正在学习中
- `deviceName`：当前学习的设备名称
- `progress`：学习进度，格式为"已学习数/总数"
- `currentCommand`：当前正在学习的指令名称
- `learnedCount`：已学习的指令数量
- `totalCount`：总指令数量
- `learnedCommands`：已学习的指令名称列表

**学习流程：**
1. 发送`start_learn`指令启动学习
2. 轮询`query_learn_status`查询状态
3. 当`learnedCount`增加时，表示当前信号学习成功，可以继续下一个信号
4. 当`learnedCount == totalCount`时，表示所有信号学习完成
5. 发送`query_learn_result`获取学习结果

**8. 停止设备学习：**
```json
{
  "action": "stop_learn"
}
```

**9. 查询学习结果：**
```json
{
  "action": "query_learn_result"
}
```

**响应**（SmartIR兼容格式）：
```json
{
  "status": "success",
  "message": "{\"deviceName\":\"...\",\"commands\":{...}}",
  "timestamp": 1234567890
}
```

**10. 获取最近接收的信号：**
```json
{
  "action": "get_recent_signals"
}
```

**11. 发送红外信号：**

HTTP API和MQTT接口使用完全统一的格式，都直接接收字符串格式（不进行JSON包装）。

**格式1：Base64字符串**（纯字符串）
```
发送到主题：homeassistant/orvibo-ct30w/command

Payload（纯Base64字符串）：
Base64字符串
```

**格式2：数组字符串**（纯字符串）
```
发送到主题：homeassistant/orvibo-ct30w/command

Payload（数组字符串）：
[8991,-4546,553,-578,...]
```

**格式3：协议格式字符串**（纯字符串）
```
发送到主题：homeassistant/orvibo-ct30w/command

Payload（协议格式字符串）：
1,0xFF00FF00,1
```
- 格式：`protocol,data,repeatCount`
- protocol：协议编号（1=NEC, 2=RC5, 3=RC6, 4=Sony, 5=Samsung）
- data：协议数据（十六进制格式，如0xFF00FF00）
- repeatCount：重复次数（默认1）

**格式4：SignalID对象格式**（保留现有格式，JSON格式）
```json
{
  "action": "send_signal",
  "signal": {
    "protocol": 1,
    "data": 12345,
    "hash": 0,
    "isProtocol": true
  },
  "repeatCount": 1
}
```

**说明**：
- HTTP API和MQTT接口完全统一格式
- 如果payload是纯字符串，按字符串格式解析（Base64/数组/协议）
- 如果payload是JSON格式，按JSON格式解析（SignalID对象格式）
- 字符串格式可以直接使用学习结果或SmartIR JSON配置文件中的commands字段值
- SignalID对象格式保留向后兼容

**12. 设置转发启用状态：**
```json
{
  "action": "set_forwarding_enabled",
  "enabled": true
}
```

**状态响应格式：**
所有指令执行后，设备会在 `state` 主题发布响应：
```json
{
  "status": "success",  // 或 "error", "executing"
  "message": "操作结果描述或数据",
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
    - `recent_signal`：最近接收的信号信息
- **传感器**：最近接收信号（`orvibo_ct30w_recent_signal`）
  - 状态：最近接收信号的协议类型
  - 属性：信号详细信息（协议、数据、是否匹配等）
- **开关**：启用转发（`orvibo_ct30w_forwarding_enabled`）
- **文本传感器**：日志信息（`orvibo_ct30w_log`）

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
      "action": "query_learn_result"
    }
```

响应会在 `homeassistant/orvibo-ct30w/state` 主题中发布。

**2. 发送红外信号：**
```yaml
service: mqtt.publish
data:
  topic: homeassistant/orvibo-ct30w/command
  payload: |
    {
      "action": "send_signal",
      "signal": {
        "protocol": 1,
        "data": 12345,
        "hash": 0,
        "isProtocol": true
      },
      "repeatCount": 1
    }
```

**3. 控制转发开关：**
转发开关实体可以直接在 Home Assistant 界面中操作，也可以通过服务调用：
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
    condition:
      condition: template
      value_template: "{{ trigger.payload_json.recent_signal.matched == false }}"
    action:
      - service: mqtt.publish
        data:
          topic: homeassistant/orvibo-ct30w/command
          payload: |
            {
              "action": "get_recent_signals"
            }
```

#### 统一信号格式

所有信号使用统一的 `SignalID` 结构：

```json
{
  "protocol": 1,      // 协议类型：1=NEC, 2=RC5, 3=RC6, 4=Sony, 5=Samsung
  "data": 12345,      // 协议数据（标准协议解析出的数据值）
  "hash": 0,          // 时序特征哈希（无法匹配标准协议时使用）
  "isProtocol": true  // 是否为标准协议：true=标准协议，false=使用hash匹配
}
```

此格式在以下场景中统一使用：
- 日志记录中的信号
- 转发规则中的A信号和B信号
- API接口中的信号参数
- 最近接收信号记录
- MQTT消息中的信号数据

**信号序列化**：
- `IRParser::signalToJSON()`：将信号转换为JSON字符串
- `IRParser::signalFromJSON()`：从JSON字符串解析信号

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

- **转发延迟**：≤ 20ms（单次信号，A信号接收后B信号发射的最大延迟）
- **转发延迟（重复信号）**：≤ 100ms（电源键等需要重复码的信号）
- **间隔误差**：≤ 20ms（连续信号间隔的最大误差）
- **信号缓存**：最多 5 个（支持并发接收的短时间多信号）
- **内存使用**：≤ 90%（运行时内存占用控制，超过90%时自动清理）
- **协议解析准确率**：> 99%（使用IRremoteESP8266库，解析稳定可靠）
- **时序精度**：微秒级（红外信号时序控制精度）

### 性能优化实现

#### 内存优化
- **静态内存分配**：红外接收缓冲区使用静态数组，避免动态分配带来的内存碎片
- **缓冲区大小优化**：时序数组从500减小到300，节省约1.6KB内存
- **内存监控工具**：提供`MemoryMonitor`类实时监控内存使用情况
- **移除OTA功能**：完全移除OTA功能，释放约5-10KB内存（包括ESP8266HTTPUpdate库的静态内存占用）
- **JSON文档大小优化**：减少API响应中的JSON文档大小，降低动态内存分配峰值
- **Web服务器延迟启动**：启动后8秒延迟判断，MQTT成功则不启动Web服务器，节省5-10KB内存
- **智能内存清理**：每60秒检查一次，超过90%且连续两次检查都超过阈值时才清理，避免频繁清理
- **日志保存优化**：只在日志数量达到阈值（20条）或错误日志时才保存，减少Flash写入

#### 转发延迟优化
- **异步消息队列**：使用消息队列解耦接收和转发流程，避免阻塞
- **快速返回机制**：转发过程中快速检查并返回，减少不必要的等待
- **队列处理优化**：队列处理间隔从10ms优化到5ms，提升响应速度
- **延迟时间优化**：
  - 单次信号（音量、静音）：从40ms优化到20ms（提升50%）
  - 重复信号（电源）：从150ms优化到100ms（提升33%）
  - 重复码间隔：从70ms优化到50ms
- **IRsend库集成**：NEC协议使用IRremoteESP8266库的IRsend直接发送，时序更准确，性能更好
- **减少调试输出**：移除不必要的Serial输出，减少阻塞时间

#### 间隔误差优化
- **硬件定时器**：使用ESP8266 Ticker库实现毫秒级精准延时（误差≤20ms）
- **微秒级精度**：使用优化的`delayMicroseconds`实现微秒级时序控制
- **中断管理**：优化中断关闭时间，确保不超过10ms限制

