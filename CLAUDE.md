# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

ORVIBO CT30W 红外转发器固件，基于 ESP8266 实现红外信号接收、学习、转发和 Home Assistant MQTT 集成。

### web/ 目录说明

`web/` 是一个独立的单页前端应用（SPA），**不部署在设备上**（设备 Flash 空间有限）。用户在本地浏览器中打开 `web/index.html`，通过 HTTP API 与设备交互。

- 设备 IP 由用户在"设置"页手动填写，保存在浏览器 `localStorage` 中
- 所有操作（查看信号、学习、规则管理、发送信号）均通过 `http://<device-ip>/api/...` 完成
- **WiFi / MQTT 配网不在此页面处理**，配网入口是设备 AP 模式下的嵌入式页面（`src/network/web_server.cpp` 的 `getHTML()`，访问地址 `192.168.4.1`）

## 开发命令

### 编译与烧录
```bash
# 编译固件
pio run

# 编译并上传到设备
pio run --target upload

# 串口监控（波特率 115200）
pio device monitor

# 清理编译产物
pio run --target clean
```

编译后的固件位于：`.pio/build/esp12e/firmware.bin`

### 依赖管理
```bash
# 安装/更新依赖库
pio pkg install

# 列出已安装的库
pio pkg list
```

## 核心架构

### 分层设计

```
硬件层 (hardware/)
  ├─ ir_receiver: GPIO5 红外接收（中断驱动）
  ├─ ir_transmitter: GPIO14 红外发射（PWM）
  ├─ led_control: GPIO15 状态指示
  └─ button_handler: GPIO4 按钮输入

红外核心层 (ir_core/)
  ├─ unified_signal: 统一信号格式（协议/时序码）
  ├─ ir_parser: 信号解析（使用 IRremoteESP8266）
  ├─ ir_mapper: A→B 映射规则管理
  ├─ ir_forwarder: 转发逻辑（消息队列异步）
  ├─ ir_generator: 信号生成与发射
  └─ ir_learner: 学习模式

网络层 (network/)
  ├─ wifi_manager: WiFi 连接与 AP 配网
  ├─ mqtt_client: MQTT 客户端
  └─ web_server: HTTP API（条件启动）

接口层 (ha_interface/)
  └─ ha_mqtt_handler: Home Assistant MQTT Discovery

工具层 (utils/)
  ├─ message_queue: 异步消息队列
  ├─ storage: Flash 持久化（LittleFS）
  ├─ logger: 日志管理（最近3条）
  ├─ memory_monitor: 内存监控
  ├─ precise_timer: 非阻塞定时器
  └─ unified_api: 统一 API 处理
```

### 信号格式

**UnifiedSignal（单个信号）**：
- 协议信号：`protocol` + `data`（如 NEC: 1, 0x00003039）
- 时序码信号：`timingBase64` + `timingCount`
- `useTiming` 标志区分类型

**SignalSequence（信号序列）**：
- `signals[]`：信号数组（最多5个）
- `intervals[]`：间隔数组（毫秒）
- 支持字符串格式：`signal1@30;signal2@50|2`（序列重复2次）

### 性能关键点

**转发延迟优化**：
- 目标：A 信号接收到 B 信号发射 ≤100ms
- 红外发射任务间隔：5ms
- 转发前延时：50ms（AB 信号隔离，避免干扰接收器导致目标设备无响应）
- 非阻塞主循环（使用 `yield()` 替代 `delay()`）

**内存管理**：
- 运行时内存占用目标 ≤90%
- 静态分配优先（避免碎片）
- 时序缓冲区 300 点（类成员变量，避免栈溢出）
- 智能清理：30秒检查，连续两次超85%才清理
- MQTT 缓冲区：8192 字节（支持大型 JSON 响应）
- 最近信号记录：5 条
- 日志条目：最多 10 条

**中断控制**：
- 发射时关闭中断 ≤10ms
- 每 100 个时序点检查一次（减少开销）

## 重要配置文件

- `src/config/constants.h`：缓冲区大小、超时阈值
- `src/config/pins.h`：GPIO 引脚定义
- `platformio.ini`：编译选项、依赖库、串口配置

## API 接口

### HTTP REST API
仅在以下情况启动：
- AP 模式
- MQTT 未配置或连接失败
- Discovery 发布失败

主要端点：
- `GET /api/status` - 设备状态
- `GET /api/maps` - 查询映射规则
- `PUT /api/maps` - 更新映射（单个/批量）
- `POST /api/learn/start` - 启动学习
- `GET /api/learn/status` - 学习进度
- `POST /api/signals/send` - 发送信号

### MQTT 接口
- 指令主题：`homeassistant/orvibo-ct30w/command`
- 状态主题：`homeassistant/orvibo-ct30w/state`
- SmartIR 主题：`smartir/+/+/send`（自动订阅）

支持的 action：
- `query_maps`, `update_map`, `get_logs`
- `start_learn`, `query_learn_status`, `stop_learn`, `query_learn_result`
- `get_recent_signals`, `send_signal`
- `set_forwarding_enabled`, `get_status`

## 开发注意事项

### 内存约束
- ESP8266 总 RAM 约 80KB，可用约 40KB
- 避免大对象栈分配（栈空间约 4KB）
- 大缓冲区使用类成员或静态变量
- JSON 文档使用前检查可用内存

### 字符串处理
- MQTT 回调避免 String 对象，使用 C 字符串
- 减少动态内存分配
- 优先使用 `strncpy`, `snprintf` 等安全函数

### 协议解析
- 使用 IRremoteESP8266 库解析标准协议
- 未匹配协议使用时序特征哈希
- 相同按键每次接收协议数据一致

### 信号记录
- 所有信号输出到串口和 `recentSignals` 缓冲区
- 不写入日志缓冲区（减少转发开销）
- 最多保留最近 3 条信号

### 测试
- 使用串口监控查看实时日志
- LED 状态指示：慢闪（AP）、快闪（连接中）、关闭（就绪）、闪烁（接收）、长亮（发射）
- 通过 `/api/signals/recent` 查询最近信号

## 常见问题

**编译错误**：
- 缺少库：运行 `pio pkg install`
- 内存不足：检查 `constants.h` 缓冲区大小

**上传失败**：
- 检查串口号（platformio.ini 中的 `upload_port`）
- 降低 `upload_speed`
- 手动进入下载模式：按住 BOOT，按 RESET，松开 BOOT

**运行问题**：
- WiFi 连接失败：确认 2.4GHz 网络
- MQTT 连接失败：检查服务器地址和认证
- 红外无响应：检查 GPIO 引脚连接
- 映射不生效：查看串口日志，检查信号匹配

## 文档参考

- `README.md`：项目简介和快速开始
- `docs/getting-started.md`：快速开始指南
- `docs/user-guide.md`：用户使用指南
- `docs/signal-format.md`：信号格式说明
- `docs/troubleshooting.md`：故障排查
- `docs/requirements.md`：项目需求和实现思路
- `docs/architecture/ir-forwarding.md`：红外转发逻辑详解
- `docs/architecture/ir-learning.md`：红外学习逻辑详解
- `docs/architecture/smartir-mqtt.md`：SmartIR MQTT 接口规范
