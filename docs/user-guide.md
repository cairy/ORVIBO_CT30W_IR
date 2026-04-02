# 用户指南

## 目录
- [红外信号学习](#红外信号学习)
- [配置映射规则](#配置映射规则)
- [Web 配置界面](#web-配置界面)
- [Home Assistant 集成](#home-assistant-集成)
- [SmartIR 使用](#smartir-使用)
- [转发开关控制](#转发开关控制)

---

## 红外信号学习

学习功能可录制任意遥控器的按键信号，学习结果可直接用于配置映射规则或导入 SmartIR。

### 学习流程

**方式 1：串口读取（最简单）**

1. 连接串口监控：`pio device monitor`
2. 用遥控器对准设备按键
3. 串口输出信号信息：
   ```
   [receive] Protocol:1, Data:11B08F7, Matched:No
   ```
4. 记录 `Protocol` 和 `Data` 值，用于配置映射规则

**方式 2：API 批量录制**

适合批量录制多个按键信号：

```bash
# 1. 启动学习模式
POST http://设备IP/api/learn/start
{
  "deviceName": "空调遥控器",
  "commandNames": "开机,关机,温度+,温度-,模式",
  "timeoutSeconds": 30
}

# 2. 轮询学习状态
GET http://设备IP/api/learn/status
# 返回：{"isLearning": true, "progress": "2/5", "currentCommand": "温度+"}

# 3. 每次 progress 变化，对准设备按下对应按键
# 4. 学习完成后获取结果
GET http://设备IP/api/learn/result
```

学习结果以 SmartIR 兼容的 JSON 格式返回，可直接导入 SmartIR 配置。

### 信号查询

查询最近接收到的信号（最多保留 3 条）：
```bash
GET http://设备IP/api/signals/recent
```

---

## 配置映射规则

映射规则定义了"收到 A 信号 → 发送 B 信号"的转发逻辑。

### 字符串格式（推荐）

简洁易读，适合手动配置：
```bash
PUT http://设备IP/api/maps
{
  "id": 1,
  "rule": {
    "name": "音量+",
    "a": "1,0x011B08F7",
    "b": "1,0xFD22DD"
  }
}
```

- `a`：A 信号，格式为 `协议ID,信号值`（协议 ID：1=NEC, 2=RC5, 3=RC6, 4=Sony, 5=Samsung）
- `b`：B 信号，同上格式，支持信号序列（详见[信号格式说明](signal-format.md)）
- `name`：规则名称（可选，便于识别）

### B 信号序列（带间隔）

当目标设备需要多个信号时，使用序列格式：
```bash
{
  "id": 2,
  "rule": {
    "name": "AV切换",
    "a": "1,0x011B7689",
    "b": "1,0xFD48B7@40;1,0x00FD48B7"
  }
}
```

`1,0xFD48B7@40;1,0x00FD48B7` 表示：先发送信号 `0xFD48B7`，等待 40ms，再发送 `0x00FD48B7`。

### 批量更新规则

一次性更新所有规则（会清空现有规则）：
```bash
PUT http://设备IP/api/maps
{
  "rules": [
    {"name": "音量+", "a": "1,0x011B08F7", "b": "1,0xFD22DD"},
    {"name": "音量-", "a": "1,0x011B8877", "b": "1,0xFDC23D"},
    {"name": "静音",  "a": "1,0x011B708F", "b": "1,0xFD708F"},
    {"name": "电源",  "a": "1,0x011B26D9", "b": "1,0xFDB04F"}
  ]
}
```

### 查询现有规则

```bash
GET http://设备IP/api/maps
```

---

## Web 配置界面

Web 界面在以下情况下可用：
- AP 模式（立即启动）
- Wi-Fi 已连接但 MQTT 未配置
- MQTT 连接失败

访问地址：`http://设备IP/` 或 AP 模式下的 `http://192.168.4.1/`

> **注意**：MQTT 正常连接且 Home Assistant Discovery 发布成功后，Web 服务器会自动停止以节省内存（约 5-10KB）。

---

## Home Assistant 集成

### 自动发现

设备连接 MQTT 后会自动推送 Discovery 配置，Home Assistant 会自动创建以下实体：

| 实体类型 | 实体 ID | 说明 |
|----------|---------|------|
| 二进制传感器 | `orvibo_ct30w_status` | 设备在线状态 |
| 传感器 | `orvibo_ct30w_state` | 设备状态（转发、学习、规则数等） |
| 开关 | `orvibo_ct30w_forwarding_enabled` | 启用/禁用转发功能 |
| 文本传感器 | `orvibo_ct30w_log` | 最新日志 |

### 在自动化中发送红外信号

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
      }
    }
```

### 控制转发开关

```yaml
service: mqtt.publish
data:
  topic: homeassistant/orvibo-ct30w/command
  payload: "true"   # 或 "false" 禁用
```

### 查询最近接收的信号

```yaml
service: mqtt.publish
data:
  topic: homeassistant/orvibo-ct30w/command
  payload: '{"action": "get_recent_signals"}'
```

响应发布到 `homeassistant/orvibo-ct30w/state` 主题。

### Discovery 失败处理

如果设备未在 Home Assistant 中显示：
1. 检查串口输出是否有 `[HA Discovery]` 相关日志
2. 确认 MQTT 集成已启用自动发现（设置 > 设备与集成 > MQTT）
3. 设备会自动以递增间隔重试（1秒、5秒、10秒、20秒）

---

## SmartIR 使用

设备自动订阅以下 SmartIR 主题：

| 主题 | 设备类型 |
|------|----------|
| `smartir/climate/+/send` | 空调、暖气 |
| `smartir/media_player/+/send` | 电视、音响 |
| `smartir/fan/+/send` | 风扇 |
| `smartir/light/+/send` | 灯光 |

SmartIR 发送的 payload 会被设备自动解析，支持协议格式、Base64 格式和时序码格式。

---

## 转发开关控制

默认情况下转发功能已启用。可通过以下方式控制：

- **Home Assistant 界面**：直接点击转发开关实体
- **MQTT 命令**：
  ```json
  {"action": "set_forwarding_enabled", "enabled": false}
  ```
- **HTTP API**：（暂无专用接口，通过 MQTT 控制）
