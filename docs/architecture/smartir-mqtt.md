# SmartIR MQTT 接口规范文档

## 概述

本文档说明 SmartIR 通过 MQTT 发送红外命令时的接口规范和负载格式。

## 当前实现

根据 SmartIR 源码分析（`controller.py`）：

- SmartIR 的 MQTT 控制器**仅支持 Raw 格式**（`commandsEncoding: "Raw"`）
- **SmartIR 完全按照 JSON 配置文件中的 `commands` 字段的值发送 MQTT payload**
- MQTTController 直接将 JSON 文件中的命令值作为 payload 发送，**不做任何转换、不添加任何包装**
- 命令值从 JSON 文件的 `commands` 字段读取，原样通过 MQTT publish 发送

### 关键点

1. **完全按配置发送**：如果配置文件中是 `"off": "Base64字符串"`，则发送 `Base64字符串`
2. **如果配置文件中是 `"off": "[8991,-4546,...]"`，则发送 `[8991,-4546,...]`**
3. **如果配置文件中是 `"off": "{\"ir_code_to_send\": \"Base64字符串\"}"`，则发送 `{"ir_code_to_send": "Base64字符串"}`**
4. **SmartIR 不会自动添加任何包装**：不会添加 JSON 包装、不会添加前缀后缀
5. **学习结果输出格式**：学习结果输出格式应该直接就是 SmartIR JSON 配置文件中 `commands` 字段的值格式
6. **设备接收端**：设备接收 MQTT 消息时，需要根据实际格式进行解析

## JSON 文件配置

在 SmartIR 的 JSON 配置文件中，需要设置：

```json
{
  "supportedController": "MQTT",
  "commandsEncoding": "Raw",
  "commands": {
    "off": "...",
    "on": "..."
  }
}
```

## 完整配置示例

### 示例 1：Climate（空调）设备

#### JSON 配置文件（1286.json）

```json
{
  "manufacturer": "Fujitsu",
  "supportedModels": ["AR-JE5"],
  "commandsEncoding": "Raw",
  "supportedController": "MQTT",
  "minTemperature": 18,
  "maxTemperature": 30,
  "precision": 1,
  "operationModes": ["auto", "cool", "dry", "fan_only", "heat"],
  "fanModes": ["auto", "high", "med", "low", "quiet"],
  "commands": {
    "off": "{\"ir_code_to_send\": \"B4oMRQbCAY0BgAMDvQTCAUALB70EjQGNAcIBgAMEjQG9BMIgAwWNAY0BwgGAAwSNAb0EwiADBY0BjQHCAYADQAHAC+AHB0ABwBMDjQG9BMAL4AMHQAGAD+ALHwe9BI0BjQHCAYADQAEJwgGNAcIBjQHCAQ==\"}",
    "auto": {
      "auto": {
        "18": "{\"ir_code_to_send\": \"B+UMXga/AZQBQAMDlAHEBEAHAb8BwAcAlGABAb8BQA8HxASUAZQBvwGAAwSUAcQEvyADBZQBlAG/AYADwAFAD0ABQAfgDQMExAS/AZRgAUAH4AcDAZQBgB9AF8ABQAsBvwFAFwLEBJQgA0AHwAMBlAFAAQK/AZQgAQG/AYAPAb8BgAPAAeABD0AJAsQEv2ADAJSgAUAPAJRgAUAHQAMBlAHgBRMBlAGAFYAFAZQBwAcBlAHgBQnADUAHQAPAAeADCwGUAYANgAUBlAHABwGUAeAFCcANQAdAA8ABwAtABwC/IAEDlAHEBEADAL8gB0ADAZQBQAELxAS/AZQBlAHEBJQB\"}",
        "19": "{\"ir_code_to_send\": \"CS8NQQaXAZcBwAFAAwXDBJcBlwHAB0ABAcABQAMBwwSAAwCXYAFABwTAAcMElyADAsABl2ABQAdAA8ABAMAgAQCXoAFACUADwAEBwAGANwHAAYADQAHgBQvgBR8CwAGXYAEEwAHDBJegA8ALQAcEwAHAAZegAQHAAUAPAJdgAUAHQANAAUAHQAOAAUAjQAMBlwGAA0ALAsABl+AAAUAPB8MElwGXAcABgANAAUAL4AEB4AENAMAgAQCX4AAB4A0LQBVAA8AB4AMLAZcB4AMN4A8LAZcB4A8ZAcABQKsBwwTAB0ALDZcBlwGXAZcBwAHDBJcB\"}"
      }
    }
  }
}
```

#### Home Assistant 配置（configuration.yaml）

```yaml
smartir:
  check_updates: false

climate:
  - platform: smartir
    name: 客厅空调
    unique_id: living_room_ac
    device_code: 1286
    controller_data: smartir/climate/living_room_ac/send
```

#### SmartIR 发送的 MQTT 消息示例

**场景 1：关闭空调**

- **Topic**: `smartir/climate/living_room_ac/send`
- **Payload**: 
  ```
  {"ir_code_to_send": "B4oMRQbCAY0BgAMDvQTCAUALB70EjQGNAcIBgAMEjQG9BMIgAwWNAY0BwgGAAwSNAb0EwiADBY0BjQHCAYADQAHAC+AHB0ABwBMDjQG9BMAL4AMHQAGAD+ALHwe9BI0BjQHCAYADQAEJwgGNAcIBjQHCAQ=="}
  ```

**场景 2：设置自动模式，温度 18 度，自动风速**

- **Topic**: `smartir/climate/living_room_ac/send`
- **Payload**: 
  ```
  {"ir_code_to_send": "B+UMXga/AZQBQAMDlAHEBEAHAb8BwAcAlGABAb8BQA8HxASUAZQBvwGAAwSUAcQEvyADBZQBlAG/AYADwAFAD0ABQAfgDQMExAS/AZRgAUAH4AcDAZQBgB9AF8ABQAsBvwFAFwLEBJQgA0AHwAMBlAFAAQK/AZQgAQG/AYAPAb8BgAPAAeABD0AJAsQEv2ADAJSgAUAPAJRgAUAHQAMBlAHgBRMBlAGAFYAFAZQBwAcBlAHgBQnADUAHQAPAAeADCwGUAYANgAUBlAHABwGUAeAFCcANQAdAA8ABwAtABwC/IAEDlAHEBEADAL8gB0ADAZQBQAELxAS/AZQBlAHEBJQB"}
  ```

### 示例 2：Media Player（媒体播放器）设备

#### JSON 配置文件（7460.json）

```json
{
  "manufacturer": "ZTE",
  "supportedModels": ["B860H"],
  "supportedController": "ESPHome",
  "commandsEncoding": "Raw",
  "commands": {
    "off": "[8991, -4546, 553, -578, 553, -1709, 553, -578, 552, -579, 553, -1709, 553, -1709, 553, -578, 553, -1708, 554, -1708, 553, -579, 552, -1708, 554, -1709, 553, -578, 552, -579, 552, -1709, 553, -578, 553, -580, 551, -579, 552, -1685, 577, -1709, 553, -1708, 553, -579, 552, -1708, 554, -1709, 553, -1708, 553, -1709, 554, -578, 552, -579, 552, -579, 552, -1710, 552, -579, 552, -578, 553]",
    "on": "[8991, -4546, 553, -578, 553, -1709, 553, -578, 552, -579, 553, -1709, 553, -1709, 553, -578, 553, -1708, 554, -1708, 553, -579, 552, -1708, 554, -1709, 553, -578, 552, -579, 552, -1709, 553, -578, 553, -580, 551, -579, 552, -1685, 577, -1709, 553, -1708, 553, -579, 552, -1708, 554, -1709, 553, -1708, 553, -1709, 554, -578, 552, -579, 552, -579, 552, -1710, 552, -579, 552, -578, 553]",
    "volumeUp": "[8995, -4492, 584, -599, 531, -1730, 532, -601, 530, -601, 529, -1732, 530, -1730, 532, -600, 531, -1730, 532, -1730, 531, -601, 531, -1707, 555, -1729, 532, -602, 529, -601, 530, -1730, 533, -601, 528, -602, 530, -599, 532, -600, 530, -600, 531, -601, 530, -601, 530, -601, 530, -1731, 531, -1730, 532, -1729, 533, -1730, 531, -1707, 555, -1731, 531, -1706, 556, -1730, 532, -600, 531]"
  }
}
```

#### Home Assistant 配置（configuration.yaml）

```yaml
smartir:
  check_updates: false

media_player:
  - platform: smartir
    name: 客厅电视
    unique_id: living_room_tv
    device_code: 7460
    controller_data: smartir/media_player/living_room_tv/send
```

#### SmartIR 发送的 MQTT 消息示例

**场景 1：关闭电视**

- **Topic**: `smartir/media_player/living_room_tv/send`
- **Payload**: 
  ```
  [8991, -4546, 553, -578, 553, -1709, 553, -578, 552, -579, 553, -1709, 553, -1709, 553, -578, 553, -1708, 554, -1708, 553, -579, 552, -1708, 554, -1709, 553, -578, 552, -579, 552, -1709, 553, -578, 553, -580, 551, -579, 552, -1685, 577, -1709, 553, -1708, 553, -579, 552, -1708, 554, -1709, 553, -1708, 553, -1709, 554, -578, 552, -579, 552, -579, 552, -1710, 552, -579, 552, -578, 553]
  ```

**场景 2：音量增加**

- **Topic**: `smartir/media_player/living_room_tv/send`
- **Payload**: 
  ```
  [8995, -4492, 584, -599, 531, -1730, 532, -601, 530, -601, 529, -1732, 530, -1730, 532, -600, 531, -1730, 532, -1730, 531, -601, 531, -1707, 555, -1729, 532, -602, 529, -601, 530, -1730, 533, -601, 528, -602, 530, -599, 532, -600, 530, -600, 531, -601, 530, -601, 530, -601, 530, -1731, 531, -1730, 532, -1729, 533, -1730, 531, -1707, 555, -1731, 531, -1706, 556, -1730, 532, -600, 531]
  ```

### 示例 3：协议格式（扩展支持）

#### JSON 配置文件（自定义格式）

```json
{
  "manufacturer": "Custom",
  "supportedModels": ["TV-001"],
  "supportedController": "MQTT",
  "commandsEncoding": "Raw",
  "commands": {
    "power": "NEC,0xFF00FF00,32",
    "volumeUp": "NEC,0xFF10EF,32",
    "volumeDown": "NEC,0xFF20DF,32",
    "channelUp": "NEC,0xFF30CF,32"
  }
}
```

#### Home Assistant 配置（configuration.yaml）

```yaml
smartir:
  check_updates: false

media_player:
  - platform: smartir
    name: 自定义电视
    unique_id: custom_tv
    device_code: 9999
    controller_data: smartir/media_player/custom_tv/send
```

#### SmartIR 发送的 MQTT 消息示例

**场景：电源开关**

- **Topic**: `smartir/media_player/custom_tv/send`
- **Payload**: 
  ```
  NEC,0xFF00FF00,32
  ```

**说明**：接收端需要解析逗号分隔的字符串，提取协议类型、编码值和位数。

## MQTT Payload 格式

**核心原则**：SmartIR 的 MQTTController **完全按照 JSON 配置文件中的 `commands` 字段的值发送 MQTT payload，不做任何转换、不添加任何包装**。

### 发送机制

SmartIR 发送 MQTT 消息的方式：

1. **读取 JSON 配置**：从 JSON 文件的 `commands` 字段读取命令值
2. **原样发送**：将命令值作为字符串 payload 直接发送，不做任何处理
3. **无包装**：不会自动添加 JSON 包装、不会添加前缀后缀

### 格式对应关系

| JSON 配置文件中的值 | SmartIR 发送的 Payload | 说明 |
|-------------------|---------------------|------|
| `"off": "Base64字符串"` | `Base64字符串` | 直接发送 Base64 字符串 |
| `"off": "[8991,-4546,...]"` | `[8991,-4546,...]` | 直接发送数组字符串 |
| `"off": "{\"ir_code_to_send\": \"Base64字符串\"}"` | `{"ir_code_to_send": "Base64字符串"}` | 直接发送 JSON 字符串 |

**重要**：
- Payload **可以是任意字符串**，完全由 JSON 文件定义者决定
- SmartIR 不强制任何特定格式
- **学习结果输出格式应该直接就是 SmartIR JSON 配置文件中 `commands` 字段的值格式**
- 接收端需要根据实际格式进行解析

### 常见格式示例

虽然 SmartIR 不限制格式，但实际使用中常见以下格式：

#### 格式 1：JSON 字符串

Payload 是一个 JSON 字符串，包含红外码数据：

```json
{
  "ir_code_to_send": "B4oMRQbCAY0BgAMDvQTCAUALB70EjQGNAcIBgAMEjQG9BMIgAwWNAY0BwgGAAwSNAb0EwiADBY0BjQHCAYADQAHAC+AHB0ABwBMDjQG9BMAL4AMHQAGAD+ALHwe9BI0BjQHCAYADQAEJwgGNAcIBjQHCAQ=="
}
```

**实际发送的 payload**：上述 JSON 的字符串形式（在 JSON 文件中需要转义为 `"{\"ir_code_to_send\": \"...\"}"`）

**JSON 文件中的写法**：
```json
{
  "commands": {
    "off": "{\"ir_code_to_send\": \"B4oMRQbCAY0BgAMDvQTCAUALB70EjQGNAcIBgAMEjQG9BMIgAwWNAY0BwgGAAwSNAb0EwiADBY0BjQHCAYADQAHAC+AHB0ABwBMDjQG9BMAL4AMHQAGAD+ALHwe9BI0BjQHCAYADQAEJwgGNAcIBjQHCAQ==\"}"
  }
}
```

#### 格式 2：数组字符串

Payload 是一个数组字符串，表示原始时序数据：

```
[8991, -4546, 553, -578, 553, -1709, 553, -578, 552, -579, 553, -1709, ...]
```

**实际发送的 payload**：上述数组的字符串形式

**JSON 文件中的写法**：
```json
{
  "commands": {
    "off": "[8991, -4546, 553, -578, 553, -1709, 553, -578, 552, -579, 553, -1709, ...]"
  }
}
```

#### 格式 3：协议格式（扩展支持）

如果接收端支持，也可以是协议格式：

```
NEC,0xFF00FF00,32
```

**注意**：SmartIR 本身不处理这种格式，需要接收端自行解析。

**JSON 文件中的写法**：
```json
{
  "commands": {
    "power": "NEC,0xFF00FF00,32"
  }
}
```

## MQTT 接口对应关系

### Topic 配置

- **Topic 来源**：Home Assistant 配置中的 `controller_data` 字段
- **示例**：`controller_data: smartir/climate/living_room_ac/send` → Topic 为 `smartir/climate/living_room_ac/send`

### Payload 对应关系

SmartIR **完全按照 JSON 配置文件中的 `commands` 字段的值发送**，不做任何转换：

| JSON 配置文件中的值 | SmartIR 发送的 Payload | 说明 |
|-------------------|---------------------|------|
| `"off": "Base64字符串"` | `Base64字符串` | 直接发送，无包装 |
| `"off": "[8991,-4546,...]"` | `[8991,-4546,...]` | 直接发送数组字符串 |
| `"off": "{\"ir_code_to_send\": \"Base64字符串\"}"` | `{"ir_code_to_send": "Base64字符串"}` | 直接发送 JSON 字符串 |
| `"off": "NEC,0xFF00FF00,32"` | `NEC,0xFF00FF00,32` | 直接发送协议格式字符串 |

**关键点**：
- SmartIR **不会自动添加任何包装**
- 学习结果输出格式应该直接就是 SmartIR JSON 配置文件中 `commands` 字段的值格式
- 设备接收 MQTT 消息时，需要解析这些格式

## MQTT 接口实现

### 基本要求

1. **Topic**：由 SmartIR 配置中的 `controller_data` 字段指定
2. **Payload**：字符串类型，可以是任意格式
3. **QoS**：建议使用 QoS 1 或 2 确保消息送达

### 接口处理逻辑

接收端需要：

1. **订阅指定 Topic**
2. **解析 Payload**（根据实际使用的格式）：

   - 如果使用 JSON 格式：解析 JSON，提取 `ir_code_to_send` 字段
   - 如果使用数组格式：解析数组字符串，获取时序数据
   - 如果使用协议格式：按逗号分隔，提取 protocol、code、bits
   - 如果使用自定义格式：按自定义规则解析

3. **发送红外信号**：根据解析出的数据发送红外码

### 示例代码

#### Python 示例

```python
import json
import base64
import ast

def on_mqtt_message(topic, payload):
    """
    处理 SmartIR 发送的 MQTT 消息
    
    Args:
        topic: MQTT topic
        payload: 字符串类型的 payload
    """
    # payload 是字符串类型
    try:
        # 尝试解析为 JSON 对象
        data = json.loads(payload)
        if "ir_code_to_send" in data:
            ir_code = data["ir_code_to_send"]
            # Base64 解码
            ir_data = base64.b64decode(ir_code)
            # 发送红外码
            send_ir_raw(ir_data)
            return
    except json.JSONDecodeError:
        pass
    
    # 尝试解析为数组字符串（时序数据）
    try:
        if payload.startswith('[') and payload.endswith(']'):
            ir_timing = ast.literal_eval(payload)
            # 发送时序数据
            send_ir_timing(ir_timing)
            return
    except (ValueError, SyntaxError):
        pass
    
    # 尝试解析为协议格式（逗号分隔）
    if ',' in payload:
        parts = payload.split(',')
        if len(parts) == 3:
            protocol = parts[0].strip()
            code = parts[1].strip()
            bits = int(parts[2].strip())
            # 发送协议格式红外码
            send_ir_protocol(protocol, code, bits)
            return
    
    # 如果都不匹配，记录错误
    print(f"无法解析的 payload 格式: {payload}")
```

#### C/C++ 示例（伪代码）

```cpp
void onMqttMessage(const char* topic, const char* payload) {
    // 尝试解析 JSON
    if (payload[0] == '{') {
        // 解析 JSON，提取 ir_code_to_send
        // Base64 解码后发送
        return;
    }
    
    // 尝试解析数组格式
    if (payload[0] == '[') {
        // 解析数组字符串，获取时序数据
        // 发送时序数据
        return;
    }
    
    // 尝试解析协议格式
    if (strchr(payload, ',')) {
        // 按逗号分隔，提取 protocol, code, bits
        // 发送协议格式红外码
        return;
    }
}
```

## 扩展：协议格式支持（可选）

虽然当前 SmartIR 不支持协议格式，但如果需要扩展支持，可以：

1. **修改 SmartIR 源码**：在 `MQTTController.send()` 中添加协议格式处理
2. **或实现兼容层**：在接收端解析逗号分隔字符串

协议格式示例：

```
NEC,0xFF00FF00,32
```

解析方式：

```python
parts = payload.split(',')
protocol = parts[0]  # "NEC"
code = parts[1]      # "0xFF00FF00"
bits = int(parts[2]) # 32
```

## 注意事项

### 核心原则

1. **SmartIR 完全按配置发送**：SmartIR 完全按照 JSON 配置文件中的 `commands` 字段的值发送 MQTT payload，不做任何转换、不添加任何包装
2. **学习结果格式**：学习结果输出格式应该直接就是 SmartIR JSON 配置文件中 `commands` 字段的值格式
3. **无自动包装**：SmartIR 不会自动添加 JSON 包装、不会添加前缀后缀，完全原样发送

### 格式说明

4. **格式对应关系**：
   - 如果配置文件中是 `"off": "Base64字符串"`，则发送 `Base64字符串`
   - 如果配置文件中是 `"off": "[8991,-4546,...]"`，则发送 `[8991,-4546,...]`
   - 如果配置文件中是 `"off": "{\"ir_code_to_send\": \"Base64字符串\"}"`，则发送 `{"ir_code_to_send": "Base64字符串"}`

5. **Payload 是字符串类型**：不是二进制数据，需要接收端自行解析
6. **格式约定**：虽然 SmartIR 不限制格式，但建议使用统一的格式以便接收端处理
7. **JSON 文件中的转义**：如果 payload 是 JSON 字符串，在 JSON 文件中需要正确转义（`\"`）

### 接收端要求

8. **设备接收端**：设备接收 MQTT 消息时，需要根据实际格式进行解析，支持多种格式的兼容解析
9. **Topic 配置**：Topic 由 SmartIR 配置中的 `controller_data` 字段指定，例如：`smartir/climate/living_room_ac/send`

## 参考

- SmartIR 源码：`custom_components/smartir/controller.py`
- MQTTController 实现：第 135-152 行
- 示例 JSON 文件：`SmartIR-master/codes/climate/1286.json`（JSON 格式）、`SmartIR-master/codes/media_player/7460.json`（数组格式）
