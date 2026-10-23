# ORVIBO CT30W 红外转发器

<p align="center">
  <strong>基于 ESP8266 的智能红外信号学习与转发设备</strong><br>
  支持 Home Assistant MQTT 自动发现 | SmartIR 兼容 | 红外信号学习
</p>

---

## 项目简介

ORVIBO CT30W 是一款基于 ESP8266 的红外转发器固件，可以学习、存储和转发红外信号，实现：

- **红外信号学习**：录制任意遥控器的按键信号
- **A→B 信号映射**：用一个遥控器控制另一个设备（例如用电视遥控器控制空调）
- **Home Assistant 集成**：通过 MQTT 自动发现，无需手动配置
- **SmartIR 兼容**：支持 SmartIR 的 MQTT 接口，可直接使用 SmartIR 的设备配置文件
- **Web 配置界面**：通过浏览器配置 Wi-Fi、MQTT 和映射规则

---

## 核心特性

✅ **红外信号学习与转发**  
录制遥控器信号，支持 NEC、RC5、RC6、Sony、Samsung 等标准协议，以及非标准时序码

✅ **A→B 信号映射**  
将一个遥控器的按键映射到另一个设备的信号，实现跨设备控制

✅ **Home Assistant MQTT 自动发现**  
连接 MQTT 后自动在 Home Assistant 中创建设备实体，无需手动配置

✅ **SmartIR 兼容**  
支持 SmartIR 的 MQTT 主题和信号格式，可直接使用社区的设备配置文件

✅ **Web 配置界面**  
AP 模式下提供 Web 界面，配置 Wi-Fi、MQTT 和映射规则

✅ **信号序列支持**  
支持发送多个信号的序列，可设置信号间隔和重复次数

✅ **低延迟转发**  
A 信号接收到 B 信号发射延迟 ≤100ms，实时响应

✅ **内存优化**  
针对 ESP8266 的内存限制进行深度优化，运行稳定可靠

---

## 快速开始

### 1. 硬件准备
- ORVIBO CT30W 设备（或基于 ESP8266 ESP-12E 的兼容硬件）
- USB 转串口适配器（首次烧录用）

### 2. 烧录固件
```bash
# 安装 PlatformIO
pip install platformio

# 克隆仓库
git clone https://github.com/你的用户名/ORVIBO_CT30W_IR.git
cd ORVIBO_CT30W_IR

# 编译并烧录
pio run --target upload

# 串口监控（可选）
pio device monitor
```

详细步骤请参考 [快速开始指南](docs/getting-started.md)。

### 3. 首次配置
设备无配置时自动进入 AP 模式：
- SSID：`ORVIBO-CT30W`
- 密码：编译时从 `secrets.ini` 的 `ap_password` 注入（没有默认值，见 [secrets.example.ini](secrets.example.ini)）
- 配网页面：http://192.168.4.1

在配网页面填写家庭 Wi-Fi 和 MQTT 服务器信息。

### 4. Home Assistant 集成
设备连接 MQTT 后会自动在 Home Assistant 中创建设备实体：
- 二进制传感器：设备在线状态
- 传感器：设备状态（学习模式、转发状态、规则数量等）
- 开关：启用/禁用转发功能
- 文本传感器：最新日志

无需手动配置，设备会自动推送 MQTT Discovery 配置。

---

## 使用场景

### 场景 1：用电视遥控器控制空调
1. 学习电视遥控器的音量+/音量-信号（A 信号）
2. 学习空调遥控器的温度+/温度-信号（B 信号）
3. 配置映射规则：音量+ → 温度+，音量- → 温度-
4. 现在可以用电视遥控器调节空调温度

### 场景 2：Home Assistant 自动化
1. 在 Home Assistant 中创建自动化
2. 触发条件：时间、传感器状态等
3. 动作：通过 MQTT 发送红外信号
4. 实现定时开关空调、电视等设备

### 场景 3：SmartIR 集成
1. 在 Home Assistant 中安装 SmartIR 集成
2. 使用社区的设备配置文件（如空调、电视）
3. 设备自动订阅 SmartIR 的 MQTT 主题
4. 无需额外配置即可使用

---

## 文档

- [快速开始](docs/getting-started.md) - 硬件准备、烧录、首次配置
- [用户指南](docs/user-guide.md) - 信号学习、映射配置、Home Assistant 集成
- [API 参考](docs/api-reference.md) - HTTP 和 MQTT 接口详细说明
- [信号格式说明](docs/signal-format.md) - UnifiedSignal 和 SignalSequence 格式
- [开发指南](docs/development.md) - 编译、架构、贡献指南
- [故障排查](docs/troubleshooting.md) - 常见问题和解决方案
- [刷机与 OTA](docs/flashing-and-ota.md) - 三种写固件方式、串口 AT、常见坑
- [压测与长稳测试](docs/stress-testing.md) - 判据与阈值、工具用法、什么时候要抓串口

### 架构文档
- [红外转发逻辑](docs/architecture/ir-forwarding.md)
- [红外学习逻辑](docs/architecture/ir-learning.md)
- [SmartIR MQTT 接口规范](docs/architecture/smartir-mqtt.md)

### 参考（非主固件）
- [独立 Arduino 红外转换草图](reference/ir-converter-finally/README.md) — 历史/实验示例（ESP32-C3），与 `pio run` 主工程无关

---

## 技术规格

| 项目 | 说明 |
|------|------|
| 芯片 | ESP8266 ESP-12E |
| 内存 | 80KB RAM（启动期可用堆约 21KB，稳态空闲约 12KB） |
| Flash | 2MB（Puya P25Q16，链接脚本 `eagle.flash.2m256.ld`） |
| Wi-Fi | 2.4GHz 802.11 b/g/n |
| 红外接收 | GPIO5 |
| 红外发射 | GPIO14 |
| 状态 LED | GPIO15 |
| 按钮 | GPIO4 |
| 转发延迟 | ≤100ms |
| 支持协议 | NEC, RC5, RC6, Sony, Samsung |
| 最大映射规则 | 20 条 |
| 最大学习指令 | 8 个 |

---

## 性能指标

- **转发延迟**：≤100ms（A 信号接收到 B 信号发射）
- **转发前延时**：20ms（AB 信号隔离，避免干扰）
- **协议解析准确率**：>99%（使用 IRremoteESP8266 库）
- **时序精度**：微秒级
- **内存使用**：≤90%（运行时自动清理）
- **信号缓存**：最多 3 个

---

## 贡献

欢迎提交 Issue 和 Pull Request！

### 开发环境
- PlatformIO IDE（推荐 VS Code）
- ESP8266 工具链

### 代码规范
- 遵循现有代码风格
- 提交前运行 `pio run` 确保编译通过
- 添加必要的注释和文档

详见 [开发指南](docs/development.md)。

---

## 许可证

本项目采用 MIT 许可证。详见 [LICENSE](LICENSE) 文件。

---

## 致谢

- [IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266) - 红外协议解析库
- [ArduinoJson](https://arduinojson.org/) - JSON 解析库
- [PubSubClient](https://github.com/knolleary/pubsubclient) - MQTT 客户端库
- [Home Assistant](https://www.home-assistant.io/) - 智能家居平台
- [SmartIR](https://github.com/smartHomeHub/SmartIR) - 红外设备集成

---

## 联系方式

- GitHub Issues: [提交问题](https://github.com/你的用户名/ORVIBO_CT30W_IR/issues)
- 讨论区: [GitHub Discussions](https://github.com/你的用户名/ORVIBO_CT30W_IR/discussions)

---

<p align="center">
  Made with ❤️ for Home Assistant Community
</p>
