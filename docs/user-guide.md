# 用户指南

## 目录
- [红外信号学习](#红外信号学习)
- [配置映射规则](#配置映射规则)
- [Web 配置界面](#web-配置界面)
- [页面上没有按钮的接口](#页面上没有按钮的接口)
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

> ⚠️ **学习结果只存在设备内存里**（`/api/learn/result` 读的就是它，没有落盘）—— 重启、
> 断电、升级固件、设备自愈都会**静默丢掉**。所以学完要立刻处理，三选一：
> 在面板「学习」页对每条点**「设为 B」**建规则、点**「保存到设备」**，
> 或者切到**「设备」页**用顶部那条提示**一键加入**目标设备。

### 信号查询

查询最近接收到的信号（最多保留 5 条）：
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

**只要设备连上 WiFi，Web 界面就一直可用**（v1.2.0 起）。不用再管 MQTT 连没连上、
Home Assistant 有没有发现到设备 —— 打开就是。

| 页面 | 地址 | 用途 |
| --- | --- | --- |
| 面板 | `http://设备IP/` | 状态总览、转发总开关、最近信号、学习、规则、设备 |
| 配网 | `http://设备IP/setup` | 搜索 WiFi、填 WiFi / MQTT、回退到上次能联网的配置 |
| 安全模式 | `http://设备IP/` | 只在设备进安全模式时出现（整页琥珀色），见下 |
| 固件上传 | `http://设备IP/update` | **只在安全模式可用**；业务模式下会刷崩设备，见下一节 |

AP 模式下地址是 `http://192.168.4.1/`。

> **什么时候会打不开**：只有设备内存快用尽时才会自动把界面下线（省内存），
> 内存回落后自动恢复。这种情况少见；真遇到了，断电重启一次即可。

### 第一次配网（一部手机就能做完）

1. 设备没配过网、或连不上原来那个 WiFi 时会开热点：名称 `ORVIBO-CT30W`，密码是你编译时在 `secrets.ini` 里填的 `ap_password`。
2. 手机连上这个热点，浏览器打开 `http://192.168.4.1/`。
   面板此时顶部会有一条橙色「配网模式」横幅，下面就是**「去配网」按钮**，点它直接进配网页。
3. 配网页里点**「搜索」**列出周边 WiFi，点一个就填好了名称（不用手打）；再填密码。
4. 点「保存并重启设备」。设备 LED 快闪 3 下表示收到了，随后重启并连新网络。
5. 手机上把 Wi-Fi 切回同一个网络（设备热点这时已经没了），以后用 `http://设备IP/` 打开面板。

几条会反复用到的规则：

- **留空 = 不改动**：密码、用户名、端口留空都不会被清掉。只想加/改 MQTT 时，
  不用再重新输一遍 WiFi 密码。
- **换了 WiFi 又把密码留空** = 告诉设备"这个网络没有密码（开放网络）"，页面会先弹一次确认，
  免得漏填把设备弄下线。
- **要关掉 MQTT**：在 MQTT 分区里勾「关闭 MQTT」再保存（只清空服务器地址不会生效，那是"不改动"）。
- **配错了怎么救**：配网页上的「回退到上次能联网的配置」会恢复到**最后一次真正连上 WiFi 的那份配置**
  并重启；串口下 `AT+RESTORE` 是同一个动作。

### 安全模式页

设备**连续几次启动失败**，或**上电时按钮被按住**，就会进安全模式。这个页面的职责很窄：
只加载联网 + 固件升级，红外转发 / 规则 / MQTT 一律不加载。页面整页琥珀色警示带，
和正常面板的绿色「运行中」横幅一眼可分，只提供两件事：

- **重新刷一个固件**：点「打开固件上传页」用浏览器传 `.pio/build/esp12e/firmware.bin`
  （用户名 `admin`，口令见项目 `secrets.ini`），或命令行
  `pio run -t upload --upload-port <设备IP>`。
- **只是误入**（比如上电时手按住了按钮）：点「立即重启回正常模式」。放着一动不动也行，
  10 分钟无操作会自动重启回去。

**指示灯慢闪（约 1 秒一次）= 处在安全模式**，这是不打开网页也能判断的办法。

---

## 页面上没有按钮的接口

面板上几乎所有操作都有按钮。设备另外还开着两条**不给界面入口**的路径 —— 不是隐藏功能，
只是不需要天天用，所以没在界面上占位置。写在这里，免得需要的时候找不到：

| 想干什么 | 怎么进 | 说明 |
| --- | --- | --- |
| 看设备自己的系统日志 | 浏览器地址栏输入 `http://设备IP/api/logs` | 直接出一段文字。记的是「设备启动」「读取 Maps 文件，大小: 1704 bytes」「开始学习设备」这类事件，最多留 10 条 |
| 浏览器刷固件 | `http://设备IP/update` | ⚠️ **业务模式下会刷崩设备、固件还不会更新**；只在安全模式下才是可用通道，见下 |

其余接口（状态、规则、最近信号、转发记录、学习、设备，以及转发总开关）在面板上**都有**对应按钮，
不需要记地址。完整清单、每个字段的含义、以及**每个接口对应界面上的哪个按钮**，见
`docs/api-reference.md` 的 §1 那张表。

**为什么 `/update` 在业务模式下不能用**：上传时要一块 2100 字节的连续内存，而设备联网后
空闲内存只剩 4 KB 上下，于是传到一半就崩（现象是设备白重启一次、固件没变）。
它正确的用场只有一个 —— **设备进安全模式时**（整页琥珀色的那个页面）点上面的
「打开固件上传页」。安全模式不加载红外转发 / 规则 / 日志，空闲内存有 17 KB，
实测 584 KB 的镜像 12 秒刷完。日常刷固件一律走命令行的无线升级（见 `docs/flashing-and-ota.md`）。

> ⚠️ 顺便说清一件容易误会的事：**这些接口默认没有密码**（只有 `/update` 有）。
> 能访问到设备 IP 的人，就能改规则、发红外、清掉配置。要加锁得在开发机上往 `secrets.ini`
> 填 `api_password` —— 代价是所有不带凭据的脚本（包括压测脚本）都会 401，取舍看你。

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

实体 ID **不会**因固件升级而变（卡片和自动化不用重建）。但"信息"那个实体上的**属性名**
在 v1.3.19 统一成了驼峰：`ruleCount`（原 `rule_count`）、`learnMode` / `learnDevice` /
`learnProgress`（原 `learn_mode` / `learn_device` / `learn_progress`）、
`forwardingEnabled`（原 `forwarding_enabled`）。用
`{{ state_attr('sensor.orvibo_ct30w_state', 'ruleCount') }}` 这类模板取值的地方要跟着改。

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
3. 设备会自动以递增间隔重试（5 秒 → 10 秒 → 20 秒，之后固定每 20 秒一次）

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
- **HTTP API**：`POST /api/forwarding`，body 是 `{"enabled":false}`。
  面板「状态」页那个开关走的就是它，重启后仍保持。
