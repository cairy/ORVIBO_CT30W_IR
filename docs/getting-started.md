# 快速开始

## 硬件准备

### 所需材料
- ORVIBO CT30W 设备（基于 ESP8266 ESP-12E 模组）
- USB 转串口适配器（首次烧录用）
- 红外接收头：GPIO5
- 红外发射管：GPIO14
- 状态 LED：GPIO15
- 按钮（出厂重置）：GPIO4

### 接线说明
设备出厂已焊接好引脚，通常无需额外接线。若自行搭建，参考 `src/config/pins.h` 中的引脚定义。

---

## 编译与烧录

### 环境准备
1. 安装 [VS Code](https://code.visualstudio.com/) 和 [PlatformIO IDE](https://platformio.org/install/ide?install=vscode) 扩展
2. 验证安装：`pio --version`

### 编译
```bash
# 命令行
pio run

# VS Code：点击底部状态栏 "✓" 图标，或 Ctrl+Alt+B
```

### 烧录固件
```bash
# 命令行
pio run --target upload

# VS Code：点击底部状态栏 "→" 图标，或 Ctrl+Alt+U
```

> **提示**：若烧录失败，按住设备 BOOT 按钮，再按一次 RESET，松开 BOOT 后重试。

### 串口监控
```bash
pio device monitor   # 波特率 115200
```

固件位置：`.pio/build/esp12e/firmware.bin`

---

## 首次配置

### 步骤 1：连接设备 Wi-Fi

设备无配置时自动进入 **AP 模式**（LED 慢闪）：
- SSID：`ORVIBO-CT30W`
- 密码：`12345678`
- 配网页面：[http://192.168.4.1](http://192.168.4.1)

在配网页面填写家庭 Wi-Fi 的 SSID、密码，以及 MQTT 服务器地址（如有）。

也可使用串口 AT 指令配置：
```
AT+WIFI=你的SSID,你的密码
AT+MQTT=服务器地址,端口,用户名,密码
```

### 步骤 2：确认设备上线

配置保存后设备自动重启。LED 状态说明：

| LED 状态 | 含义 |
|----------|------|
| 慢闪（500ms） | AP 模式，等待配网 |
| 快闪（200ms） | 正在连接 Wi-Fi / MQTT |
| 熄灭 | 正常待机 |
| 闪烁一次 | 接收到红外信号 |
| 常亮 | 正在发射红外信号 |

设备连接 MQTT 后会自动向 Home Assistant 推送 Discovery 配置，无需手动添加。

### 步骤 3：配置第一条转发规则

通过 Web 界面（AP 模式或 MQTT 不可用时）或 MQTT 配置映射规则：

```json
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

其中 `a` 是遥控器发出的信号（A 信号），`b` 是要转发的目标信号（B 信号）。

---

## 获取信号值

在没有已知信号值的情况下，使用**信号学习**功能获取：

1. 打开串口监控：`pio device monitor`
2. 用遥控器对准设备按键，串口会输出收到的信号：
   ```
   [receive] Protocol:1, Data:11B08F7, Matched:No
   ```
3. 将 `Data` 值（`0x011B08F7`）填入映射规则的 `a` 字段

或使用学习 API 批量录制（详见[用户指南](user-guide.md#红外信号学习)）。

---

## 出厂重置

长按设备按钮（GPIO4）**5 秒**，LED 快速闪烁 10 次后，设备清除所有配置并重启，重新进入 AP 模式。
