# 故障排查

本文档列出常见问题及解决方案。

---

## 编译与烧录问题

### 找不到库文件
**症状**：编译时提示缺少头文件或库

**解决方案**：
```bash
pio pkg install
```

### 内存不足
**症状**：编译时提示内存不足

**解决方案**：
检查 `src/config/constants.h` 中的缓冲区大小配置，确保未超出 ESP8266 的内存限制。

### 找不到串口
**症状**：烧录时提示找不到串口设备

**解决方案**：
1. 检查 USB 驱动是否正确安装（CH340/CP2102）
2. 检查串口号是否正确（Linux: `/dev/ttyUSB0`，Windows: `COM3`）
3. 修改 `platformio.ini` 中的 `upload_port` 配置

### 上传超时
**症状**：烧录时一直等待，最终超时

**解决方案**：
1. 手动进入下载模式：按住 BOOT 按钮，按一次 RESET，松开 BOOT
2. 降低烧录速度：修改 `platformio.ini` 中的 `upload_speed`（如改为 `115200`）
3. 检查串口连接是否稳定

---

## Wi-Fi 连接问题

### Wi-Fi 连接失败
**症状**：设备一直处于 AP 模式或 LED 快闪

**解决方案**：
1. 确认 SSID 和密码正确
2. 确认路由器支持 2.4GHz（ESP8266 不支持 5GHz）
3. 检查路由器是否开启 MAC 地址过滤
4. 尝试靠近路由器测试
5. 查看串口输出的错误信息

### 连接后频繁断线
**症状**：设备连接后不久就断开

**解决方案**：
1. 检查路由器信号强度
2. 检查路由器是否设置了客户端隔离
3. 检查路由器 DHCP 租期设置
4. 尝试为设备分配静态 IP

### ⭐ 设备"活着但连不上"（僵死态）—— v1.3.36 前只能拔电

**症状**（跟上面两条都不同，先按这个表认一下）：

| 检查项 | 僵死态的表现 |
| --- | --- |
| 串口 | `[heap]` 每 60 s 照常打一行，`AT+QUERY` 仍回 `WiFi mode: STA(connected)` |
| `ping <设备IP>` | 「无法访问目标主机」 |
| `arp -a` | 列表里**没有**这个 IP |
| 浏览器 / `curl` | HTTP 000（连接不上，不是 4xx/5xx） |
| ~~`espota` 刷机~~ | ~~`No Answer`~~ ⛔ **别用它判**（见下方注） |
| 串口 `[wifi] 断开 reason=` | **一次都没打** —— 802.11 层自认为在线 |

一句话：**主循环在跑 + 802.11 自认在线 + TCP/HTTP 全死 + 不重启 + 不自愈。**

> ⛔ **别拿 `espota` 报 `No Answer` 当证据**（2026-10-04 实测更正）：
> 健康设备对 OTA 邀请的**回应率就只有约 50%**，而且成功/失败**严格交替**
> （连测 8 次是 `回应 → 无回应 → 回应 → 无回应 → 回应`）。
> 所以单次 `No Answer` **说明不了任何事** —— 既不能证明僵死，也不能证明 OTA 挂了。
> 想验 OTA 通道请用 `tools/ota_probe.py`（内部会重试最多 5 轮）；
> **刷机时第一次 `No Answer`，直接重跑一次就好，千万别据此去拔电。**

**v1.3.36 起会自己恢复**：每 30 s 向网关做一次 TCP 探测，连续 3 次耗时超过 112 ms
（满超时 150 ms）⇒ 先写现场快照，再 `ESP.restart()`。最多连做 3 次，之后只保命不再重启。

**取证**（自愈的价值就在这一步）：

```bash
curl http://<设备IP>/api/diag/freeze          # 取现场（probes[] 是最近 8 次探测的耗时）
curl -X DELETE http://<设备IP>/api/diag/freeze # 取证完务必清掉，否则下次分不清新旧
curl http://<设备IP>/api/status                # 看 netRestartCount / netRestartedByNetwork / hasFreezeSnapshot
```

**如果还没恢复**（3 次自愈用完 / 跑的是旧固件）：只能断电重启。
⛔ **别用 `AT+RESTORE` 当重启** —— 它会拿备份配置**覆盖**主配置，坏了反而退回 AP 模式。

**已知未解**：触发机理尚未找到，约 **1 次 / 300~500 次连点红外发送**，偶发。
已排除的方向：不是堆不足（僵死时堆还有 6 KB）、不是某一路 API（同一组配置一次 7 秒死、
一次 432 次安然）、不是并发本身。真出一次时请把 `/api/diag/freeze` 的原文留下来。

---

## MQTT 连接问题

### MQTT 连接失败
**症状**：Wi-Fi 已连接，但 MQTT 无法连接

**解决方案**：
1. 确认 MQTT 服务器地址和端口正确
2. 确认用户名和密码正确（如果启用了认证）
3. 检查 MQTT 服务器是否运行正常
4. 检查防火墙是否阻止了连接
5. 查看串口输出的 MQTT 错误代码

### Home Assistant 中看不到设备
**症状**：MQTT 已连接，但 Home Assistant 中没有设备

**解决方案**：
1. 确认 Home Assistant 的 MQTT 集成已启用自动发现
2. 检查串口输出是否有 `[HA Discovery]` 相关日志
3. 检查 Discovery 是否发布成功
4. 手动触发重新发现：Home Assistant > 设置 > 设备与集成 > MQTT > 重新配置
5. 设备会自动以递增间隔重试（1秒、5秒、10秒、20秒）

### Discovery 发布失败
**症状**：串口输出显示 Discovery 发布失败

**解决方案**：
1. 检查 MQTT 缓冲区大小（已设置为 1024 字节，`MQTT_BUFFER_SIZE`）
2. 检查可用内存（Discovery 消息较大，需要足够内存）
3. Web 服务器会自动启用，可通过 `http://设备IP` 访问进行排查
4. 设备会自动重试发布

---

## 红外功能问题

### 红外接收无响应
**症状**：用遥控器对准设备按键，串口无输出

**解决方案**：
1. 检查红外接收头是否正确连接到 GPIO5
2. 检查红外接收头供电是否正常（3.3V）
3. 检查红外接收头方向是否正确
4. 尝试更换红外接收头
5. 用手机摄像头检查遥控器是否正常发射（红外 LED 会在摄像头中显示为紫色光）

### 红外发射无效果
**症状**：设备发送信号，但目标设备无响应

**解决方案**：
1. 检查红外发射管是否正确连接到 GPIO14
2. 检查红外发射管方向是否正确（长脚为正极）
3. 检查红外发射管供电是否足够（可能需要三极管驱动）
4. 检查信号值是否正确（用串口监控录制目标设备的信号）
5. 尝试增加转发前延时（`FORWARD_DELAY_BEFORE_SEND_MS`，当前为 50ms）
6. 检查目标设备是否在红外发射管的有效范围内（通常 5-10 米）

### 映射规则不生效
**症状**：配置了映射规则，但转发不工作

**解决方案**：
1. 检查规则格式是否正确
2. 检查 A 信号是否匹配（用串口监控查看接收到的信号值）
3. 检查转发功能是否启用（默认启用）
4. 查看串口日志，确认是否有匹配和转发的日志
5. 检查 B 信号是否正确（可以先用 API 直接发送 B 信号测试）

### 部分按键无法触发目标设备
**症状**：有些按键可以正常转发，有些按键目标设备无响应

**解决方案**：
1. 检查转发前延时是否足够（当前为 50ms，可在 `src/config/constants.h` 中调整）
2. 检查目标设备是否需要特殊的信号序列（如多个信号组合）
3. 尝试增加信号重复次数（`repeatCount`）
4. 检查目标设备是否对信号间隔有特殊要求

---

## 内存问题

### 内存不足导致重启
**症状**：设备运行一段时间后自动重启

**解决方案**：
1. 检查内存使用情况（串口输出会显示内存信息）
2. 设备会自动清理内存（超过 85% 且连续两次检查都超过阈值）
3. 减少映射规则数量（最多 20 条）
4. 减少学习指令数量（最多 8 个）
5. 如果 MQTT 正常连接，Web 服务器会自动停止以节省内存

### JSON 解析失败
**症状**：API 调用返回 JSON 解析错误

**解决方案**：
1. 检查 JSON 格式是否正确（使用 JSON 验证工具）
2. 检查 JSON 大小是否超过限制（MQTT 缓冲区 1024 字节，单条入站 payload 上限 `MQTT_MAX_INBOUND_PAYLOAD=768`）
3. 检查可用内存是否足够
4. 尝试减少 JSON 中的数据量（如批量更新时减少规则数量）

---

## 调试技巧

### 查看串口输出
```bash
C:/Users/cairy/.platformio/penv/Scripts/python.exe tools/serial_monitor.py COM4 60 --append --log tools/x.log
```

只有 penv 的 python 装了 `pyserial`。⭐ **`--append` 必带** —— 默认打开方式是 `"w"`，
重挂一次就把上一次的现场冲掉了（崩在重启前的栈只留在这份日志里，冲掉拿不回来）。

### 判断"上次为什么重启"—— 串口上有两个数，OTA 之后它们会打架

| 来源 | OTA 之后 | 真崩溃 |
| --- | --- | --- |
| bootloader 行 `rst cause:N, boot mode:(3,X)` | **`rst cause:2, boot mode:(3,7)`** | `rst cause:2, boot mode:(3,6)` |
| 固件 `[健康] 上次重启原因` | **4 (software-restart)** ✅ | 2 (exception) |
| 有没有 `Exception (N)` + 栈 | 没有 | **有** |

⇒ **以固件的那一行为准**。bootloader 那行的判别符是 `boot mode`：
`(3,7)` = OTA 后的 eboot 跳转（此时 `rst cause` 是噪音），`(3,6)` = 常规启动（两者一致）。
远程没有串口时看 `/api/status` 的 `resetReason` / `resetReasonText`，同一个值。

串口输出包含：
- 设备启动信息
- Wi-Fi 连接状态
- MQTT 连接状态
- 红外信号接收信息
- 转发日志
- 错误信息

### 信号输出格式

**协议信号**：
```
[receive] Protocol:1, Data:11B08F7, Matched:Yes, Interval:245ms
```

**时序码信号**：
```
[receive] Timing:156 points, Base64:ABC123XYZ456... (208 chars), Matched:No, Interval:312ms
```

### 检查内存使用
```cpp
Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
```

或使用 MemoryMonitor 工具类：
```cpp
#include "utils/memory_monitor.h"
MemoryMonitor::printMemoryInfo();
```

### 检查 Flash 使用
```cpp
FSInfo fs_info;
LittleFS.info(fs_info);
Serial.printf("Total: %d, Used: %d\n", fs_info.totalBytes, fs_info.usedBytes);
```

---

## 出厂重置

如果以上方法都无法解决问题，可以尝试出厂重置：

**方法 1：长按按钮**
- 长按设备按钮（GPIO4）**5 秒**
- LED 快速闪烁 10 次
- 设备清除所有配置并重启

**方法 2：串口 AT 指令**
```
AT+RESET
```

**方法 3：重新烧录固件**
```bash
pio run --target upload
```

---

## 获取帮助

如果以上方法都无法解决问题，请：

1. 收集以下信息：
   - 串口完整输出
   - 设备型号和固件版本
   - 问题复现步骤
   - 相关配置（Wi-Fi、MQTT、映射规则等）

2. 提交 Issue：[GitHub Issues](https://github.com/你的用户名/ORVIBO_CT30W_IR/issues)

3. 参与讨论：[GitHub Discussions](https://github.com/你的用户名/ORVIBO_CT30W_IR/discussions)
