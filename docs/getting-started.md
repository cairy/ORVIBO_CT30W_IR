# 快速开始

## 硬件准备

### 所需材料
- ORVIBO CT30W 设备（基于 ESP8266 **OW8266-02Q** 模组，2MB flash）
- **USB 转 TTL 模块**（CH340 / CH341 均可，须支持 3.3V）—— 首次烧录用；**首次需自行把 2.0mm 排针焊到主板编程测试点（出厂未焊接）**
- 红外接收头：GPIO5
- 红外发射管：GPIO14
- 状态 LED：GPIO15
- 按钮（出厂重置）：GPIO4

### 接线说明（烧录用）
主板预留一排 **2.0mm 间距编程测试点**（丝印 `3V3 / GND / RST / GPIO0 / RX / TX`，出厂没焊排针）。烧录接线：

| CT30W 测试点 | USB-TTL | 说明 |
|---|---|---|
| `3V3` | `VCC`(3.3V) | 由 USB-TTL 给板子供电，跳线拨 3.3V，勿接 5V |
| `GND` | `GND` | 共地 |
| `RX` | `TX` | 交叉 |
| `TX` | `RX` | 交叉 |
| `GPIO0` | `GND` | 拉低进下载模式，刷完拔掉 |
| `RST` | 不接 | 复位时瞬时短接到 GND |

**进入下载模式**：`GPIO0` 接地后，把 USB-TTL 插电脑即上电进入。完整接线图、示意与排障见 [刷机与 OTA 全流程](flashing-and-ota.md)。应用层引脚定义见 `src/config/pins.h`。

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

> ⚠️ 本设备是手动拉低 GPIO0 进下载模式的 CH340 板子，**没有可靠自动复位电路**，`pio run --target upload`（走 esptool 默认复位）在这块板子上连不上。
> **正确做法见 [刷机与 OTA 全流程](flashing-and-ota.md)**，那里有接线图、整片备份、串口刷写与无线 OTA 的完整步骤。要点：
> 1. GPIO0 接地 + 复位进下载模式（保持 GPIO0 拉低）；
> 2. 用 `tools/flash_write.py`（已封装好"连接→擦除→写→校验"同进程）；
> 3. 刷完断开 GPIO0、复位，新固件才运行。
>
> 设备已在网且固件带 OTA 时，日常升级走无线 ArduinoOTA 即可，免接线。
> **注意本机有个坑**：`pio run -t upload` 用的是 PIO 自带的 python，它不在 Windows 防火墙的
> 入站放行名单里，会被挡（表现：PC 停在 `No response from device`）。请按
> `flashing-and-ota.md` §6 B1 的命令，用**系统 Python** 直接跑 `espota.py`。
> 另：浏览器 `/update` 通道在本机固件上会刷崩，别用。

固件位置：`.pio/build/esp12e/firmware.bin`

### 串口监控

日常看日志用 `pio device monitor`（115200）。**排障抓开机日志**时本机 CH340 有个坑：用默认电平打开串口会把设备按住复位导致全空白，请用：

```bash
C:\Users\cairy\.platformio\penv\Scripts\python.exe tools/serial_monitor.py COM4 150 tools/serial_capture.log
```

（详见 [刷机与 OTA 全流程 · §7/§9](flashing-and-ota.md)）

---

## 首次配置

### 步骤 1：连接设备 Wi-Fi

设备无配置时自动进入 **AP 模式**（LED 慢闪）：
- SSID：`ORVIBO-CT30W`
- 密码：编译时从 `secrets.ini` 的 `ap_password` 注入（没有默认值，见 `secrets.example.ini`）
- 配网页面：[http://192.168.4.1](http://192.168.4.1)

在配网页面填写家庭 Wi-Fi 的 SSID、密码，以及 MQTT 服务器地址（如有）。

也可使用串口 AT 指令配置：
```
AT+WIFI=你的SSID,你的密码
AT+MQTT=服务器地址,端口,用户名,密码
```

> **AT 指令已知坑**：`at_handler.cpp` 会把整条命令先 `toUpperCase()` 再解析，导致大小写敏感的 **SSID / 密码被强制大写存错**。若走串口配网，请暂时只用全大写或数字密码；或改 Web 配网页（已修于 1.1.2，见下）。
>
> **Web 配网页坑（已修于固件 1.1.2）**：1.1.1 及更早版本，提交按钮在 `onclick` 里把自己 `disabled`，多数手机浏览器会**吞掉表单提交**——点下去显示"保存中…"但请求没发出去、设备 LED 不闪、不重启。现象就是"配网无效"。升级到 **1.1.2+** 后正常：提交后 LED 快闪 3 下、浏览器显示"配置已保存"并 3 秒后重启。

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
