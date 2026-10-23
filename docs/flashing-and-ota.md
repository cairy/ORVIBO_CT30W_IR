# 刷机与 OTA 全流程（CT30W / ESP8266 2MB）

> 适用硬件：ORVIBO CT30W 红外转发器（主板 OW8266-02Q 模组 / ESP8266 + 2MB flash；外置 USB 转 TTL 模块 CH340/CH341 烧录用，出厂未焊排针）
> 最后校订：2026-10-02 ｜ 固件 **v1.3.23**，布局 `eagle.flash.2m256.ld`
> 本文所有步骤均经实机验证，非推测。

---

## 0. 三种"写固件"的方式，先分清

| 方式 | 何时用 | 需要接线 | 通讯通道 |
|---|---|---|---|
| **A. 串口有线刷写**（本文 §3–§5） | 首次烧录、改布局后、固件刷崩救砖 | 是（USB + GPIO0 拉低） | CH340 串口 / esptool |
| **B. 无线 OTA**（本文 §6） | 日常改固件（设备已在网、固件带 OTA） | 否 | 局域网 ArduinoOTA（TCP 8266，**唯一可用通道**） |
| **C. 安全模式恢复**（本文 §8） | 固件能上电但起不来 / 连不上网 / OTA 半途失败 | 否（仅按键：上电按住 GPIO4） | 安全模式内的 ArduinoOTA（**别用它的 `/update`**，同 §6 B2 的堆问题） |

> 设计上的铁律（来自 core 3.1.2 源码实测，勿再猜）：**ESP8266 没有分区表、没有第二可执行槽、没有 A/B 回滚**。
> app 永远只有一个、永远在 flash `0x0`，OTA 只是"把新镜像写到 FS 下方，重启后整体复制到 0x0 覆盖旧的"。
> 所以"刷崩"的最坏情况只是"需要接线"，设备不会报废——下载模式在芯片只读 ROM 里，flash 写坏多少都不影响进下载模式。

---

## 1. 硬件与工具准备

CT30W 主板焊的是一片 **OW8266-02Q**（ESP8266，2MB flash，类 WT8266-S1）。板上预留了一排**编程测试点（焊盘），出厂未焊接排针**，间距是 **2.0mm**（不是常用的 2.54mm）。刷机要自己把排针焊/插上去，再用外置 USB 转 TTL 模块连电脑。

### 所需材料
- ORVIBO CT30W 设备（已拆壳，露出主板）
- **USB 转 TTL 模块**（CH340 / CH341 均可），必须支持 **3.3V** 输出（绝不能用 5V 给 ESP 供电）
- **2.0mm 间距排针**（或掰弯 2.54mm 排针凑合焊），6 根母对公杜邦线
- 电烙铁 + 焊锡（把排针焊到测试点上；也可不焊、把杜邦公头直接插进 2.0mm 焊盘，但易接触不良）
- 镊子（进下载模式时短接 GPIO0 与 GND 用）
- 电脑已装 PlatformIO（自带 `penv` Python，含 esptool）

⚠️ **本机必须用 PlatformIO 自带的 penv 跑 esptool**：系统 Python（`Python312`）没有 `pyserial`，直接跑会缺模块。

```bash
# Windows 上写死用这个解释器（路径按实际用户名调整）
C:\Users\cairy\.platformio\penv\Scripts\python.exe
```

> 应用层引脚（来自 `src/config/pins.h`，供参考）：红外接收 GPIO5、红外发射 GPIO14、按钮 GPIO4、LED GPIO15。以下刷机用的 `GPIO0 / RX / TX / RST / 3V3` 是 ESP8266 启动/下载专用脚，与应用脚不同。

---

## 2. 接线与进入下载（Flash）模式

### 2.1 板载编程焊点
主板上那排 **2.0mm 测试点**（丝印通常标 `3V3 / GND / RST / GPIO0 / RX / TX`，顺序可能因版本略有不同，以你板子丝印为准）。出厂没焊排针，需自行焊/插。

### 2.2 接线表（CT30W ↔ USB-TTL）

| CT30W 测试点 | USB-TTL 模块 | 说明 |
|---|---|---|
| `3V3` | `VCC` / `3.3V` | **由 USB-TTL 给板子供电**。模块跳线必须拨到 3.3V，**绝不能接 5V** |
| `GND` | `GND` | 共地 |
| `RX` | `TX` | 交叉连接 |
| `TX` | `RX` | 交叉连接 |
| `GPIO0` | `GND` | **拉低以进入下载模式**；刷完拔掉 |
| `RST` | （先不接） | 复位用；需要时把 `RST` 瞬时短接到 `GND` 即可复位 |

> ⚠️ **刷机时不要同时插 CT30W 自带的 Micro USB**：板子供电来自 USB-TTL 的 3.3V，双电源会冲突。

### 2.3 接线示意
```
CT30W 主板                 USB-TTL (CH340/CH341)
┌──────────┐               ┌──────────────┐
│ 3V3 ─────┼──────────────→│ VCC (3.3V)   │
│ GND ─────┼──────────────→│ GND          │
│ RX  ─────┼──────────────→│ TX           │
│ TX  ─────┼──────────────→│ RX           │
│ GPIO0 ───┼─────┐         │              │
│          │     ↓ 短接    │              │
│ GND ─────┼─────┘(进下载) │              │
│ RST ─────┼── (不接；复位时瞬时碰 GND)  │
└──────────┘               └──────────────┘
```

### 2.4 进入下载模式的步骤（每次有线刷写都重做）
1. 按上表接好线，**GPIO0 先接到 GND**（保持拉低）。
2. 把 USB-TTL 插到电脑 USB → 板子通电、**GPIO0 为低 → 自动进入下载模式**。
3. 设备管理器确认出现串口（本机为 `COM4`；换口则把命令里的 `COM4` 换成实际口）。
4. 跑刷写脚本（§5）。**全程保持 GPIO0 接地。**

### 2.5 退出下载模式（让新固件跑起来）
- 刷写脚本跑完，设备仍停在下载模式（GPIO0 还接地）。
- **拔掉 GPIO0 与 GND 的短接**，再重新插拔一次 USB-TTL（断电→上电）→ 新固件正常启动，广播 `ORVIBO-CT30W` 热点。

### 2.6 本机 CH340 致命坑（必读）
本板子的 CH340 **只接了 TX/RX，DTR/RTS 没接到 EN**（启动/复位脚），所以**串口开关不会触发设备复位**，
esptool 也无法靠自动拉 DTR/RTS 进入下载模式。后果与处置：
- 进下载模式必须**手动把 GPIO0 接地 + 上电**（§2.4），工具侧用 `no_reset`，不依赖任何自动复位。
- 推荐用 §5 的 `flash_write.py`：它封装了「`no_reset` 连接 → 加载 stub → 整片擦除 → 写 0x0 → MD5 回读校验」，
  **全程不关串口**，把"探测 + 擦写 + 校验"放在一个进程里，避免任何需要自动复位的步骤。
- ⚠️ `serial_monitor.py --reset` 的复位脉冲在本机同样无效（DTR/RTS 没接 EN），判"有无重启"看 `uptime`，别看日志空不空（见 §7 串口判据 ②）。

### 2.7 兜底：确认哪个点是 GPIO0
若板子丝印不清、认不准 GPIO0：在只接好 `3V3/GND/RX/TX` 的前提下，**依次用镊子把每个未知测试点瞬时短接到 GND 并上电**，哪次 `flash_write.py` 能连上（`connect: 成功`）哪个就是 GPIO0。找到后固定下来即可。

---

## 3. 编译固件

```bash
# 在仓库根目录
pio run
# 固件产物：.pio/build/esp12e/firmware.bin（含 4KB bootloader，实测 587,744 B ≈ 574KB @ v1.3.23）
```

当前布局 `2m256`：`_FS_start=0x1C0000`、`_FS_end=0x1FB000`。firmware.bin 实测 587,744 B（占 app 区 56.3%，@ v1.3.23），
OTA 可上传文件上限 `1,306,624B`（余量 ~702KB），足够反复自刷。

---

## 4. 刷写前：整片备份（强烈建议）

刷机前先对整片 flash 做一次备份，**这是唯一的回滚安全网**（不占任何固件空间）：

```bash
C:\Users\cairy\.platformio\penv\Scripts\python.exe tools/backup_flash.py COM4 backup_2mb.bin
```

- 走 stub 快读，2MB 约 2–3 分钟，跑完设备仍停在下载模式，可直接接着刷。
- 备份里的 `/network_config.json` 是**明文**，万一要找回 WiFi 账号密码：
  ```bash
  grep -a '"wifi_ssid"' backup_2mb.bin
  ```
  > LittleFS 是日志式写入，**旧版本的配置仍留在 flash 上**，通常能捞到好几个历史版本。
  > 只匹配到一份（尤其是空密码）就下结论 = 漏读。

**写回整片备份**（设备被玩坏时的恢复手段，与上面的备份脚本配对）：

```bash
C:\Users\cairy\.platformio\penv\Scripts\python.exe tools/restore_flash.py COM4 backup_2mb.bin
```

- 一次性恢复**固件 + 文件系统 + WiFi/MQTT 配置 + 转发规则**（备份里全都有），不必重配网。
- 会**整片擦除**再写回 ⇒ 备份之后产生的一切变更都会丢。脚本有三道防护：
  文件必须存在、大小必须是 2MB（否则会写坏 flash 头的容量字段）、擦除前要输入 `yes`。
- 同样需要 GPIO0 接地进入下载模式；跑完断开 GPIO0 再复位才生效。

---

## 5. 串口有线刷写（方式 A）

**推荐用 `tools/flash_write.py`**，不要走 `pio run -t upload`：
`pio upload` 默认用 `default_reset` 自动复位，在这块板子上拉不进下载模式、且和 espota 配置互相打架。
`flash_write.py` 已封装好「`no_reset` 连接 → 加载 stub → 整片擦除 → 写 0x0 → MD5 回读校验」，全程不关串口。

```bash
# 设备已按 §2 进下载模式、GPIO0 保持接地
C:\Users\cairy\.platformio\penv\Scripts\python.exe tools/flash_write.py COM4 .pio/build/esp12e/firmware.bin
```

预期输出：

```
==> 连接设备...
connect   : 成功（no_reset）
chip      : ESP8266
==> 加载 stub（快读/快写）...
stub      : running
==> 整片擦除 + 写入 .pio/build/esp12e/firmware.bin ...
... Hash of data verified ...
==> 完成。设备仍停留在下载模式。
    接下来请：① 断开 GPIO0 的接地  ② 复位/重新上电  → 新固件才会真正运行。
```

✅ 看到 `Hash of data verified` = 数据写入与回读 MD5 一致，刷写成功。

> 注意：`erase_all` 会清空整个 flash，**WiFi 配置与映射表一并丢失**，首次启动 LittleFS 自动格式化，需要重新配网一次（这是布局从 4m1m→2m256 迁移的固有代价，见 `ota-plan.md` §10 F8）。

刷完后按 §2 末尾：断开 GPIO0 接地 → 复位 → 设备起 AP 热点 `ORVIBO-CT30W`，开 `http://192.168.4.1/` 配网。

---

## 6. 无线 OTA 升级（方式 B）

前提：设备已运行**带 OTA 的固件（≥1.1.x）**，且已连上你家 WiFi（STA 模式，有一个可达的局域网 IP）。

一共两条通道，但**只有 B1（ArduinoOTA）真能用**。B2（浏览器 `/update`）实测会**把设备刷崩**，已停用。

### B1. ArduinoOTA（`pio run -t upload`）

`platformio.ini` 已放开：

```ini
upload_protocol = espota
upload_flags = --auth=${secrets.ota_password}
```

口令**不在仓库里**：`[platformio] extra_configs = secrets.ini` 把它从本机 `secrets.ini`
（`.gitignore` 已排除，模板 `secrets.example.ini`）读进来，同一个值也以
`-D OTA_PASSWORD=\"${secrets.ota_password}\"` 编进固件，所以两边天然一致。
缺 `secrets.ini` 时构建直接报 `No section: 'secrets'`；口令为空则 `static_assert` 拦下。

```bash
pio run -t upload --upload-port <设备IP>
# 例：pio run -t upload --upload-port 192.168.11.111
```

### B1. ArduinoOTA —— 唯一可用通道

原理：PC 先发一个 UDP 邀请包到设备的 8266 端口，随后**设备反过来 TCP 连回 PC 上一个临时端口**。
所以 PC 端必须有**入站放行**，否则设备连不回来（表现为 PC 停在 `No response from device`，
设备串口打印 `[OTA] 失败(错误码 2): 连接失败`，错误码 2 = OTA_CONNECT_ERROR）。

本机已有一条现成的入站放行规则，但它**只针对系统 Python**：

| 项 | 值 |
|---|---|
| 规则名 | `python.exe`（入站 / TCP / 配置文件「公用」/ 动作 允许） |
| 程序路径 | `C:\Users\cairy\AppData\Local\Programs\Python\Python312\python.exe` |

**所以必须用这个 Python 跑 espota**。用 PIO 自带的 `~/.platformio/penv/Scripts/python.exe` 会被
防火墙挡掉 —— 之前所有 `pio run -t upload` 失败都是这个原因，跟设备、跟代码都无关。

2026-09-26 已验证两次成功的命令：

```bash
SYSPY="C:/Users/cairy/AppData/Local/Programs/Python/Python312/python.exe"
ESPOTA=~/.platformio/packages/framework-arduinoespressif8266/tools/espota.py
OTA_PW=$(sed -n 's/^ *ota_password *= *//p' secrets.ini)      # 口令只从 secrets.ini 取，别写进命令历史
"$SYSPY" "$ESPOTA" \
  -i 192.168.11.111 -I 192.168.11.61 -p 8266 \
  -a "$OTA_PW" \
  -f .pio/build/esp12e/firmware.bin
```

- `-I` 必须填 **PC 自己的局域网 IP**（本机 `192.168.11.61`），要跟设备同网段；填错一样连不回来。
- `-a` 就是 `secrets.ini` 里的 `ota_password`（= 固件里的 `OTA_PASSWORD`，由同一处注入，不会不一致）。
  **改口令后第一次刷写必须用旧口令** —— 设备校验的是它自己固件里那份，刷完新固件才换成新口令。
- `pio run -t upload --upload-port <IP>` 与上面等价，但它调的是 PIO 那个 python ⇒ **本机走不通**，
  除非给那个 python 也补一条入站规则。
- 上传期间别关串口监视：设备会把 `[OTA] 0%…100%` 打在串口上，这是确认它真在写盘的唯一证据。
- 若 `Authenticating...OK` 之后就卡住，先查两件事：`-I` 是否是 PC 的真实 IP；跑 espota 的 python 是否在放行规则里。

### B2. 浏览器上传通道（HTTP multipart）—— 业务模式下 ❌ 已停用，别再试

> ⚠️ **别把这条读成"这个通道废了"**：它**只在业务模式下不可用**（空闲堆不够）。
> **在安全模式下它是可用的，而且是实测过的第二救援通道**（2026-09-27：584 KB 镜像 12 秒
> 一次过、零崩溃）。判据、实测数据与用法见 **§8.2**。

`http://<设备IP>/update` 在**业务模式**下会**把设备刷崩并重启，而固件不会更新**。2026-09-26 用串口抓到了完整现场：

```
[17:40:13] Update: firmware.bin          ← 认证过了，Update.begin 也成功了
sleep disable
.[17:40:13] .[17:40:14] ..Fatal exception 29(StoreProhibitedCause):
epc1=0x4000df64, excvaddr=0x00000000, ctx: sys
```

两个原因，指向同一件事 —— **这台设备空闲堆只有约 4KB，不够 HTTP 上传用**：

1. `ESP8266WebServer.h:58` 的 `HTTP_UPLOAD_BUFLEN = 2048`，而 `_parseForm()` 开始解析上传时要
   `new HTTPUpload()`，**需要一块连续的约 2100 字节**（崩溃日志里的
   `last failed alloc call: 40214106(2100)`，反解出来正是 `ESP8266WebServerTemplate::_parseForm`）。
2. 即使改成极慢分片（实测 1024B/40ms，能写到约 170KB 才崩），WiFi 协议栈仍会在长上传中
   因为分配不到数据包缓冲而崩：`epc1 = 0x4000df64` 正落在 ROM 的 `memcpy`
   （`eagle.rom.addr.v6.ld:204: memcpy = 0x4000df48`）里，写地址为 NULL，且 `ctx: sys`
   —— 崩在系统上下文，不是我们的代码。

**对照结论**：ArduinoOTA 走裸 TCP，没有 HTTP/multipart 这层开销，**同样的 4KB 空闲堆下两次完整跑完**。
这就是"只有 B1 可用"的技术原因。`tools/ota_http.py` 保留着，但只在需要复现崩溃现场时用。

> 另外两条仍然成立的旧坑，顺手记着：
> - 裸 `curl -F ... /update` 会被本机 `http_proxy`（`127.0.0.1:14995`）劫持，必须加 `--noproxy '*'`。
> - **别在 ArduinoOTA 失败之后紧接着试任何上传通道**：`Updater::begin()` 见到 `_size > 0`
>   （上一次 ArduinoOTA 已开始但没走完 `end()`）会直接返回 false。1.2.0 起固件已加自愈
>   （ArduinoOTA 失败且 `Update.isRunning()` 时自动重启清状态）；1.1.x 遇到这种情况要断电重启一次。

### 网络前提

PC 必须和**设备同网段**。若设备还在 AP 模式（IP `192.168.4.1`），PC 得连上 `ORVIBO-CT30W` 热点才能 OTA——但 **Windows 常连不上这个开放 AP**（手机能连，是 AP 本身没问题）。遇此情况别在 Windows 上死磕，直接走 §5 串口刷写更稳。

---

## 7. 验证刷上去了没有

**最稳的是 Web 接口**（绕开串口时机问题）：

```bash
curl http://<设备IP>/api/status
```

关键字段：

| 字段 | 含义 |
|---|---|
| `firmware` | 固件版本，刷机后应与 `constants.h` 的 `FW_VERSION` 一致 |
| `sketchMD5` | 固件指纹，**应与本地 `md5sum .pio/build/esp12e/firmware.bin` 一致** |
| `resetReason` / `resetReasonText` | 上次为什么重启：`2 exception` = 崩过；`4 software-restart` = OTA / 主动重启 |
| `bootFailCount` / `bootFailLimit` | 安全模式"可疑启动计数"：**0 就是干净**，到 limit 会自动进安全模式（见 §8.2） |
| `lowMemRestarts` | 连续因堆见底主动重启的次数（稳定 5 分钟后自动清零） |
| `wifiConfigured` | WiFi 配置是否写进 LittleFS |
| `wifiMode` | `AP`（未配网）或 `STA`（已连网） |
| `wifiConnected` / `wifiSSID` / `rssi` | 联网状态、连的 SSID、信号强度 |

**串口取证**（排障时用）：`tools/serial_monitor.py` 已修过 RTS/DTR 坑——`open` 后强制 `setDTR(False)/setRTS(False)` 释放复位，否则本机 CH340 会把设备按住不打印：

```bash
C:\Users\cairy\.platformio\penv\Scripts\python.exe tools/serial_monitor.py COM4 150 --log tools/serial_capture.log
```

> 解释器只能用 **penv 那个 python**（`pyserial` 装在它里面）；而 §6 的 espota 必须用
> **系统 Python312**（只有它在防火墙放行名单里）。两个工具的解释器不是同一个，别混。
> `/` 与 `/api/status` 的并发压测用 `python tools/stress_http.py <设备IP> --workers 4 --rounds 12`，
> 判据是"**没重启**（uptime 单调递增）+ 请求零失败"；压测时务必同时抓串口，崩溃现场才拿得到。
>
> **压测的完整判据、阈值、场景矩阵，以及"什么时候真的需要抓串口"，见 `docs/stress-testing.md`。**

**长稳监控**（`tools/long_run_monitor.py`，2026-09-26 新增）——短时高压验"内存够不够"，长稳验"能不能一直不出事"：

```bash
python tools/long_run_monitor.py 192.168.11.111 --hours 24          # 默认 60 s 采样、每 30 min 取一次面板
tail -5 tools/longrun.csv                                           # 最近几次采样
cat tools/longrun.events.log                                        # 重启 / 不可达 / 指纹变化 / 堆偏低（空 = 一路平安）
```

- 负载极低（1 次/分钟），**不影响设备正常转发红外**，因此可以和开发并行；结束（或 Ctrl-C）时自己给结论。
- 判据：**零重启 + 零不可达 + 空闲堆末段不显著低于首段**（>512 B 的下滑即判疑似泄漏）。
- ⚠️ **它唯一挡的事是刷机 / 断电**：uptime 归零会让这段数据作废（CSV 是追加写的，
  续跑会接着写并用 `session` 列分段，所以中途刷机不是灾难，只是要分段看）。

**串口下 AT 命令**（改配网配置的正规通道，2026-09-26 新增 `tools/serial_at.py`）：

```bash
PENVPY="C:/Users/cairy/.platformio/penv/Scripts/python.exe"
"$PENVPY" tools/serial_at.py "AT+QUERY"                              # 回显 SSID / MQTT / 模式 / 空闲堆
"$PENVPY" tools/serial_at.py "AT+MQTT=192.168.11.61,1883,user,pass"  # 配 MQTT
"$PENVPY" tools/serial_at.py "AT+MQTT="                              # 清空 MQTT
```

**三条必须记住的串口判据**（都是实测，别再怀疑设备坏了）：

1. **设备空闲时串口一个字节都不打**。抓到的日志为空 ≠ 设备挂了；只有事件（启动 / OTA / 崩溃 /
   AT 应答 / 配网）才输出。想确认活着就 `curl /api/status`。
2. **`serial_monitor.py --reset` 的复位脉冲对本机 CH340 无效**。跑了它以后
   `uptime` 仍连续增长 ⇒ 设备根本没复位（判据：拿 `uptime` 对，别拿日志空不空对）。
   要真正重启只有两条路：**OTA 刷（哪怕刷同一个 bin）**、或断电重上电。
3. **要改 MQTT 就走串口 `AT+MQTT=`，别用 `POST /save`** —— `/save` 会拿表单里的 `ssid/password`
   **一起覆盖 WiFi**，回填错（例如照抄了过期的整片备份）就会退进 AP 配网模式（见 TASKS.md 事故记录）。
   另：`AT+MQTT=` 清空后那 1 KB 收发缓冲不会立刻归还，**重启才回到正常空闲堆**。

配网成功时串口会连续出现：

```
[配网] 收到提交: ssid长度=N, 有MQTT=否
[配网] 配置写入成功
[配网] 配置已保存，准备重启
  → 重启 → [system] 读取Maps文件，大小: 1614 bytes → WiFi connected: 192.168.x.x
```

### 7.1 MQTT 通路自测（手上没有真实 broker 时）

`tools/mqtt_test_broker.py` 是零依赖（标准库）的最小 MQTT 3.1.1 服务端，专为验证
「连接 → 订阅 → 收命令 → 回状态」这条**平时在真机上跑不到**的路径：

```bash
PENVPY="C:/Users/cairy/.platformio/penv/Scripts/python.exe"
SYSPY="C:/Users/cairy/AppData/Local/Programs/Python/Python312/python.exe"
# 1) PC 侧起 broker（务必用系统 Python312，防火墙只放行了它）
"$SYSPY" tools/mqtt_test_broker.py --port 1883 --duration 180 \
    --publish-after 20 --publish-payload '{"action":"get_status"}' \
    --username userAbc --password PassWord123        # 账号校验可选
# 2) 设备侧走串口把 MQTT 指过来（⚠️ 别用 /save，它会连 WiFi 一起写）
"$PENVPY" tools/serial_at.py "AT+MQTT=192.168.11.61,1883,userAbc,PassWord123"
# 3) 收工清空
"$PENVPY" tools/serial_at.py "AT+MQTT="
```

期望（2026-09-26 实测）：broker 打印 `<- CONNECT ... username='userAbc' password='PassWord123'`
→ `-> CONNACK (accepted)` → 设备发 HA Discovery 配置 → `SUBSCRIBE homeassistant/orvibo-ct30w/command`
与 4 条 `smartir/*/send` → 下发后 1 秒内收到 `state`（160 B，`{"success":true,...}`）。
加了 `--username/--password` 还能**顺带验证设备端没把参数大小写改坏**（见 §10 第 1 条）。
设备侧同时会打 `[MQTT] 已分配收发缓冲 1024 B（空闲堆 …）` —— 这句只在**配了 MQTT** 时出现（`MQTT_BUFFER_SIZE`，`src/config/constants.h:196`）。

---

## 8. OTA 的安全边界与故障恢复（封壳前必读）

### 8.1 结论先行：OTA **不能**保证"永远不需要串口"

ESP8266 **没有 A/B 双分区、没有回滚**。OTA 的机制是（2026-09-27 核对本地 core 3.1.2 源码）：

```
Update.begin(size)  →  暂存地址 = FS_start − round4k(size)
Update.write(...)   →  只是往暂存区写，**完全不碰正在跑的 0x0**
Update.end()        →  先比 MD5（Updater.cpp:328-335）
                       不过 ⇒ 直接 return false，**不下发拷贝命令**（旧固件继续跑）
                       通过 ⇒ eboot_command_write(ACTION_COPY_RAW, 暂存 → 0x00000)（Updater.cpp:345-350）
重启                →  eboot 执行拷贝，把暂存区覆盖到 0x0
```

所以失败分两类：

| 失败情形 | 能否空中救回 | 依据 |
|---|---|---|
| 传输中断（PC 断网 / 超时 / 拔线） | ✅ | 没走到 `end()` 就没有拷贝命令，重启回到**旧**固件 |
| 镜像 MD5 校验不过 | ✅ | 见上面 `end()` 分支 |
| 空间不足（`OTA_BEGIN_ERROR`） | ✅ | `Update.begin()` 直接失败，旧固件不受影响 |
| 口令不对（`OTA_AUTH_ERROR`） | ✅ | 同上，连写都不写 |
| 新固件启动即崩 / 被看门狗拍死 | ✅ | 见 8.2，连续 3 次 → 自动进安全模式 |
| 新固件同时占住 `/` 与 `/setup` 之外的资源 | — | `/setup` 走底层 OpenAP，无需认证（见 §10） |
| 新固件卡死在带 `delay()` 的死循环里 | ⚠️ | **不会复位** ⇒ 计数不涨 ⇒ 只能靠"上电按住 GPIO4"手动进（见 8.3 第 1 条） |
| **eboot 拷贝那零点几秒里掉电** | ❌ | `0x0` 半写坏 ⇒ 设备再也起不来 ⇒ **只有接线兜底** |

**唯一不可消除的是最后一条**：那是裸 flash 写入被中断，任何固件层面的设计都挡不住。
ESP8266 的 `0x0` 又是**唯一可执行地址**（没有 ota_0、没有 backup 分区），所以没有"另一份能跑的固件"可退。

> 反过来看：**OTA 比 PC 侧串口刷写更安全**，因为它结构上只能写"暂存区 → 0x0"，
> 写不到任意偏移；而串口 `write_flash` 填错偏移（`-fs`/偏移写错）才是经典的"一刷就砖"。
> 本项目所有刷写都只用 OTA，串口只作为最后的兜底。

**⇒ 封壳前必须做的三件事**（成本几乎为零，封死就不可逆）：

1. **壳外留 4 个测试点/排针：VCC / GND / TX / RX，并尽量把 GPIO0 也引出来。**
   （GPIO0 接地 + 复位才进下载模式；有这 4+1 个点，任何砖都能救。）
2. **把 GPIO4 按钮留在按得到的位置** —— 它是"卡死不复位"时唯一的入口（见 8.2）。
3. **刷机时保证供电稳定**，别在供电不稳 / 快断电时刷 —— 这是压低最后一条概率的唯一实操手段。

### 8.2 不靠串口的三条恢复通道（v1.3.15 加固并实测）

| # | 通道 | 触发方式 | 说明 |
|---|---|---|---|
| 1 | **自动进安全模式** | 可疑启动计数累加满 3（`SAFE_MODE_MAX_FAILS`） | 覆盖"坏固件反复重启，人手按不过来" |
| 2 | **手动进安全模式** | 上电瞬间按住 GPIO4 满 2 秒（LED 1Hz 慢闪确认） | 判定在 `setup()` 极早期，**不依赖固件是否健康** |
| 3 | **接线兜底** | GPIO0 拉低 + 复位进下载模式 | ROM 级，与 flash 内容无关，§5 必定能救 |

**通道 1 的判据（v1.3.15 起）** —— 一次启动算"失败"当且仅当下面有一条成立：
- 上一次启动的复位原因是 `hardware-watchdog` / `exception` / `software-watchdog`；**或**
- 上一次启动没走到 `setup()` 末尾。

刻意**不把主动重启算失败**：`ESP.restart()`（含 OTA 完成后的重启）报 `software-restart`，
拔电报 `power-on`，复位脚报 `external-reset`。
> 加"复位原因"这一条是为了覆盖「**`setup()` 走完、一进业务循环就崩**」的坏固件 ——
> 旧写法只认"没走完 setup"，这类固件每次都能走到末尾把计数清掉，于是永远进不了安全模式。
> **实测（2026-09-27）**：连着 OTA 刷了 8 次，`bootFailCount` 始终 `0/3`，从未误进安全模式。

**进安全模式之后**：只起最小网络 + `/`（安全模式页）+ `/safe-status` + `/update` + `/exit`，
**允许文件系统挂不上**（只读挂载意图、绝不格式化），10 分钟无操作自动重启回业务模式。
`/api/status` 的 `bootFailCount` / `bootFailLimit` 可远程确认"离安全模式还有多远"（0 = 干净）。

**两条出得来路，都已实测**（2026-09-27 演练，全程空中完成、没动硬件）：

```bash
# 路 1：espota（设备连在局域网里时首选）
"$SYSPY" "$ESPOTA" -i 192.168.11.111 -I <本机IP> -p 8266 -a "$OTA_PW" -f .pio/build/esp12e/firmware.bin

# 路 2：浏览器/命令行 multipart 上传（设备退到 AP 模式、PC 连不上热点时，用手机浏览器打开 /update）
curl --noproxy '*' -u "admin:<OTA_PW>" -F "firmware=@firmware.bin" http://<设备IP>/update
# 期望响应：Update Success! Rebooting...   串口期望：Update Success: 598784
```

⚠️ **`/update` 在安全模式下能用，在业务模式下不能用** —— 两者不是一回事（原因见 §6 B2）：
业务模式空闲堆只有约 4 KB，`_parseForm` 要一块 2100 B 连续内存，直接 `Exception 29`；
安全模式不加载 `irMapper`/`irForwarder`/`irReceiver`/`logger`，实测空闲堆 **17.3 KB**，584 KB 镜像
上传 12 秒一次过、零崩溃。**所以设备退到 AP 模式也一定有救**（手机连 `ORVIBO-CT30W` 热点 → `http://192.168.4.1/update`）。

### 8.3 三条会写坏就靠它们的纪律

1. **业务代码里禁止 `while(1)` / `for(;;)` 死等。** `delay()` 会喂饱看门狗 ⇒ 既不崩也不重启
   ⇒ 计数永远不涨 ⇒ 安全模式永远进不去 ⇒ 空中无救（只能靠通道 2 的按钮）。
   要重试就写"重试 N 次后 `ESP.restart()`"（`main.cpp` 的 LittleFS 初始化就是范例）。
   把关办法：`grep -rn "while *( *1 *)\|while *( *true *)\|for *( *;; *)" src/` ——
   只应剩 `safe_mode.cpp` 里安全模式自己那个主循环。
2. **`ESP.restart()` 之前必须继续跑 20~30 次 `server.handleClient()`**（`handleSave` /
   `handleAPIRestore`），否则响应还在 lwIP 缓冲里就被重启吞掉。
3. **改完固件先刷开壳那台跑一轮回归，再刷封壳那台。**

### 8.4 怎么在不制造真砖的前提下演练这条链路

`SAFE_MODE_TEST` 自测钩子（`main.cpp`）：伪造 3 次"上次崩溃复位"，让设备**自己**走进安全模式，
用来验证"进得去 + 还能刷得出来"。**这是唯一能安全演练救援链路的方式** ——
因为它伪造的是计数、固件本身完全健康，所以演练完一定能从空中刷回来。

```bash
# 1) 编自测版（不污染 platformio.ini，也不影响正式固件）
PLATFORMIO_BUILD_FLAGS="-DSAFE_MODE_TEST=1" pio run
cp .pio/build/esp12e/firmware.bin /tmp/fw_normal.bin     # 先把正式固件留一份
"$SYSPY" "$ESPOTA" -i <设备IP> -I <本机IP> -p 8266 -a "$OTA_PW" -f .pio/build/esp12e/firmware.bin
# 2) 验证：串口应打出 可疑启动计数 1/3 → 2/3 → 3/3 → "自动进入"，随后 /safe-status 有应答
curl --noproxy '*' http://<设备IP>/safe-status
# 3) 刷出来（espota 或 /update 任一），再编译一次正式版确保 .pio 里不是自测固件
"$SYSPY" "$ESPOTA" -i <设备IP> -I <本机IP> -p 8266 -a "$OTA_PW" -f /tmp/fw_normal.bin
pio run
```

> ⚠️ 演练完**务必**再跑一次普通 `pio run` 覆盖 `.pio` —— 否则 `.pio` 里留着的是自测固件，
> 下次 `pio run -t upload` 会把设备刷成"每 3 次启动就进安全模式"。
> 注意 `/tmp` 只对 Git-Bash 里的命令有效：`curl` 是 Windows 程序，读不了 `/tmp/...`，
> 用 `-F "firmware=@..."` 时要给 Windows 路径（否则 `curl exit 26`，请求根本没发出去）。

---

## 9. 排障速查表

| 现象 | 根因 | 处置 |
|---|---|---|
| `Timed out waiting for packet header` / 连接失败 | 设备不在下载模式，或 `default_reset` 把它踢出去了 | 确认 GPIO0 接地 + 复位；用 `flash_write.py`（no_reset）而非 `pio upload` |
| `无法连接：请确保 GPIO0 接地…` | 同上，或 COM 口不对 | 设备管理器确认端口；重做 §2 |
| OTA 报 `No response` | PC 与设备不同网段 | 先 `ping <设备IP>`；确认 PC 连的是设备所在网络 |
| OTA 报 auth 失败 | `secrets.ini` 的口令 ≠ 设备当前固件里那份 | 用设备**现在这份**固件的口令再刷一次；刷完才换成新口令（见 §6） |
| 构建报 `No section: 'secrets'` | 本机缺 `secrets.ini` | `cp secrets.example.ini secrets.ini` 并填口令（故意的，防"无口令固件"） |
| OTA `UPDATE_ERROR_SPACE` | 固件涨过布局上限 | 复算 `FS_start − round4k(getSketchSize())`；必要时缩 FS 或裁剪 |
| 点"保存配置"变成"保存中…"卡住、设备 LED 不闪 | **前端把提交掐死了**（旧固件 bug，见 §10） | 升级到 ≥1.1.2；或改用串口 AT / 安全模式上传 |
| Windows 连不上 `ORVIBO-CT30W` 热点 | 开放网络驱动协商失败（设备本身正常） | 用手机连热点，或直接走串口刷写 |
| 串口监控全空白 | 用了默认电平打开串口，CH340 把设备按住复位 | 用 `serial_monitor.py`（已置 DTR/RTS=False）；且要在复位/上电瞬间开始录 |

---

## 10. 已知坑（省得下次重踩）

1. **esptool 在本机连不上**：`SYNC_TIMEOUT=0.1` 太紧，工具里已改为 `2.0`。
2. **CH340 关串口即复位**：见 §2 致命坑，必须"探测+擦写"同进程。
3. **PC 侧无法加入 ESP8266 的 AP（"Windows 连不上设备热点"）**：设备热点**正常广播**（`netsh wlan show networks` 每轮都能扫到 `ORVIBO-CT30W`），本机也有可用 WLAN 网卡（Broadcom 802.11ac），但 `netsh wlan connect` 稳定报 `0x57`（ERROR_INVALID_PARAMETER），**删掉重建 profile 也一样**；改用 `Add-Type` 直接调 `WlanConnect` 被安全策略拦（这台机器装了管控组件）。
   ⇒ **不是设备的问题，也不是"绕开即可"**：设备一旦进 AP 模式，PC 侧救不回来，只能人工用手机连热点开 `http://192.168.4.1/setup`。所以关键是**别把设备弄进 AP 模式**（根因见第 8 条）。
4. **配网页面的提交按钮 bug（已修于 1.1.2）**：旧写法 `<button onclick="if(checkValidity()){this.disabled=true}">` 在点击时禁用按钮，多数手机浏览器会**吞掉表单提交**——按钮文字变"保存中…"但请求没发出去，设备串口无 `[配网]`、LED 不闪。表现为"配网无效、不提示成功也不重启"。修法：禁用挪到 `onsubmit` + `setTimeout(…,0)` 延后一拍。这一条是本轮"配网无效"的真正根因，与服务端保存无关。
5. **`pio run` 不接受 `--upload-flag` 全局选项**：auth 必须写进 `platformio.ini` 的 `upload_flags`。
6. **改完功能务必复算 OTA 上限**：2m1m 时固件从 482,976B 涨到 527,424B 没回头算，导致 OTA 直接 `UPDATE_ERROR_SPACE`（差 7KB）。现在 2m256 上限 `1,306,624B`、余量约 **702 KB**（v1.3.23：1,306,624 − 固件 587,744），但每次加功能都要复算。
7. **串口 AT 曾把参数强制大写（已修于 v1.3.8）**：`ATHandler::processCommand` 先整条 `toUpperCase()` 再从大写串里 `substring(8)` 取参数 ⇒ `AT+WIFI=` 的 SSID/密码里的小写字母**全被改掉**（示意：`Abc@1234xy` → `ABC@1234XY`），含小写字母的密码**永远配不上**，失败即退 AP 模式 —— 这也是设备进 AP 后串口自救失败的原因。修法：只对**命令名**做忽略大小写比较，参数取原始串。回归判据：让设备连自测 broker（`--username/--password`），broker 侧收到的账号须与命令行逐字符相同。
8. **别用 `POST /save` 改 MQTT**：它会把表单里的 `ssid/password` 与 MQTT **一起写**，"只想改 MQTT"却回填错密码（例如照抄了过期的整片备份）就会退进 AP 模式。改 MQTT 走串口 `AT+MQTT=`。另外整片备份里的 `wifi_password` 可能是**过期值**，不能当"当前值"用；但 LittleFS 是日志式写入，`grep -a '"wifi_ssid"' backup_2mb.bin` 能捞到**多个历史版本**，真密码往往就在里面。
9. **清空 MQTT 后那 1 KB 收发缓冲不会归还**：`AT+MQTT=` / 网页里清空只让设备**停止连接**，缓冲要**重启**才释放（`PubSubClient::setBufferSize` 不支持缩到 0）。表现为清空后空闲堆仍停在 ~5 KB（配 MQTT 时的常驻水位），重启才回到冷态 ~12.6 KB。
10. **串口"没输出"与 `--reset` 无效**：见 §7 的三条串口判据 —— 设备空闲时本来就不打日志；`serial_monitor.py --reset` 的复位脉冲对本机 CH340 **无效**（用 `uptime` 是否连续增长来判断，别用日志空不空）。

---

## 11. 标准迭代流程（改代码后）

```
1. 改代码
2. pio run                          # 编译，确认 Flash/RAM 不越界
3. 若设备已在网：用 §6 B1 的 espota 命令（**必须用系统 Python 跑**，见该节）
   若设备未配网/刷崩：按 §2 接线 → flash_write.py        # 串口（方式 A）
4. curl --noproxy '*' http://<IP>/api/status   # 确认 firmware 版本号变了
5. 配网/功能回归
```

> 详细的设计判据、容量推导、评审意见见 `ota-plan.md`；本文是其"可操作落地版"。
