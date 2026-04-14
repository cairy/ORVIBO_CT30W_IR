# 参考草图：红外信号转换（Arduino IDE）

本目录为**独立参考示例**，与仓库根目录的 **PlatformIO / ESP8266 主固件**（`src/`）无关。

| 项目 | 说明 |
|------|------|
| 用途 | 实验性「遥控器 A → 设备 B」NEC 码映射（音量、静音、电源、AV/TV 等） |
| 目标硬件 | **ESP32-C3**（引脚：接收 GPIO4、发射 GPIO3） |
| 构建方式 | 使用 **Arduino IDE** 打开本目录下的 `.ino`（目录名须与草图名一致） |
| 依赖 | [IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266)（库名兼容 ESP32） |

主固件请仍使用仓库根目录的 `platformio.ini` 与 `pio run`。
