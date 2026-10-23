# API 参考（HTTP + MQTT）

> 面向要写脚本 / 接 Home Assistant / 排障的人。只想"怎么用设备"请看 `user-guide.md`。
> 固件版本以 `/api/status` 的 `firmware` 字段为准；本文对应 **v1.3.19**。
> 信号本身怎么写（协议码 / 时序码 / 序列）见 `signal-format.md`，本文只讲接口。

---

## 0. 通用约定（先读这五条，能省掉一半排障）

| 项 | 说明 |
|---|---|
| 基址 | `http://<设备IP>`（AP 配网模式下是 `http://192.168.4.1`） |
| 端口 | 80（HTTP）；8266 只有 **UDP**，是 ArduinoOTA，不是 TCP 服务 |
| 成功信封 | `{"success":true,"status":"success","data":<对象/数组/字符串>,"timestamp":<毫秒>}` |
| 执行中信封 | `status":"executing"` —— 已受理但结果要另查（如发射信号） |
| 失败信封 | `{"success":false,"status":"error","error":"原因","code":<HTTP码>,"timestamp":…}` |
| CORS | 只放行**同源**（`Access-Control-Allow-Origin` 回的是请求自己的 Host），**不再是 `*`** |
| 鉴权 | 默认**关**（`secrets.ini` 的 `api_password` 留空）⇒ 除 `/update` 外全部接口匿名可达；填了就启用 Basic 认证（用户名 `admin`），**面板与每个 `/api/*` 都会 401** |

> **命名约定（v1.3.19 统一）**：JSON **字段名**一律 camelCase（`forwardingEnabled`、
> `ruleCount`、`learnMode`…），HTTP 与 MQTT 两条通道一致。
> 三种东西**不**跟着改，它们是标识而不是字段：
> ① MQTT 命令的 action 名（`set_forwarding_enabled` 等，改名会让手写自动化静默失效）；
> ② HA 的 `unique_id` / discovery 主题里的实体 id（改了 HA 会当成新实体，用户现有卡片指向旧实体）；
> ③ 设备端私有配置文件的键名（改了升级后读不出旧值）。
> `timestamp` 一律是 **`millis()` 毫秒**（相对上电，不是 Unix 时间）。

**四条容易踩的：**

1. **面板是 gzip 的，且设备不看请求头**（框架默认不收集 `Accept-Encoding`）。
   自测必须带 `curl --compressed --noproxy '*'`，否则拿到的是压缩字节。
2. **`503` / `507` 不是接口错误，是设备内存不够**。空闲堆只有 12 KB 上下，
   API 处理前会先检查内存，不够就直接拒。等一会儿重试即可，不是 bug。
3. **面板可能被内存兜底临时下线**（20 秒内自动重开）。此时 `/` 打不开但 `/api/status` 通常还在；
   判断设备死活用 `/api/status` 的 `uptime`，不要用"网页打不打得开"。
4. **默认没有密码。** 设备只防两种"从别的网页发起的调用"：Host 白名单（挡"伪装域名解析到设备 IP"，
   命中回 **403**）＋ CORS 同源收敛（挡"打开一个恶意网页、浏览器被当跳板"）。
   **这两道挡不住人直接敲地址栏或 `curl`** —— 谁能访问到设备 IP，谁就能完整接管它
   （改规则、发红外、清配置、换 WiFi）。想真加锁：在 `secrets.ini` 填 `api_password`，
   代价是所有不带凭据的脚本 / 自动化全部 401。

---

## 1. 全部端点，和「界面上有没有按钮」

下表是业务模式下注册的**全部**路由，最后一列是这份文档存在的主要理由：
绝大多数接口在面板上都有对应按钮，**只有两条没有入口** —— 它们不是"隐藏功能"，
只是不需要天天用：

| 方法 | 路径 | 干什么 | 界面上有入口吗 |
|---|---|---|---|
| GET | `/` | 设备面板（CSS/JS 已**内联**进 HTML，一次请求拿全；gzip 编进固件，随固件 OTA） | —（页面本身） |
| GET | `/favicon.ico` | 204 空响应 | 浏览器自动 |
| GET | `/setup` | 配网页（v1.3.14 起是内嵌资源，与面板共用同一套配色，见 §10） | **有**：「设置」页按钮、状态页配网横幅、无脚本提示 |
| GET | `/config` | 同上（旧路径） | 无链接（内容与 `/setup` 逐字相同） |
| POST | `/save` | 配网表单提交 —— **会同时写 WiFi 和 MQTT，见 §10.4 的警告** | **有**（配网页表单） |
| GET | `/api/wifi/scan` | 搜索周边 WiFi（发起扫描，仅 AP 模式可用，见 §10.2） | **有**（配网页「搜索」） |
| GET | `/api/wifi/scan/result` | 取扫描结果（两段式第二步，页面自动轮询） | **有**（同上，藏在「搜索」背后） |
| POST | `/api/restore` | 回退到「上次真正连上 WiFi 的配置」并重启（见 §10.3） | **有**（配网页） |
| GET | `/api/status` | 设备状态（排障第一站，见 §2） | **有**：「状态」页 |
| GET | `/api/maps` | 查全部规则（见 §3） | **有**：「规则」页 |
| PUT | `/api/maps` | 改 / 加 / 删规则 | **有**：「规则」页保存 / 删除 |
| PUT · POST | `/api/forwarding` | 转发总开关（见 §4） | **有**：「状态」页那个开关 |
| POST | `/api/learn/start` | 启动学习（见 §6） | **有**：「学习」页 |
| GET | `/api/learn/status` | 学习进度 | **有**（前端自动轮询） |
| POST | `/api/learn/stop` | 中止学习 | **有**：「学习」页 |
| GET | `/api/learn/result` | 取学习结果 | **有**：「学习」页 |
| GET | `/api/signals/recent` | 最近收到的信号（见 §7） | **有**：「最近」页 |
| POST | `/api/signals/send` | 发一条红外 | **有**（「最近」「规则」「设备」页多处按钮） |
| GET | `/api/forward/records` | 转发记录（见 §5） | **有**：「最近」页 |
| DELETE | `/api/forward/records` | 清空转发记录 | **有**：「最近」页「清空」 |
| POST | `/api/forward/test` | 试发一条 A 信号 | **有**：「最近」页「试发」 |
| GET | `/api/devices` | 读设备 / 按钮配置（**流式**，见 §8） | **有**：「设备」页 |
| POST | `/api/devices/chunk` | **存设备 / 按钮配置（分片，前端用这条）** | **有**：「设备」页 |
| POST | `/api/devices` | 存设备 / 按钮配置（整份，≤1 KB，留给外部脚本） | ❌ 前端已不用 |
| GET | `/api/diag/freeze` | **网络僵死的现场快照**（见 §14） | ❌ **没有** —— 排障专用，只有出过事才有内容 |
| DELETE | `/api/diag/freeze` | 清掉快照 | ❌ **没有** —— 取证完要手动清 |
| **GET** | **`/api/logs`** | **系统日志（环形 10 条）** | ❌ **没有** —— 页面上零引用，用法见 §9 |
| **GET · POST** | **`/update`** | **浏览器上传固件** | ❌ **没有，而且业务模式下别用** —— 见下方警告 |

> ⚠️ **`/update` 在业务模式下会把设备刷崩、固件却不会更新**：空闲堆只有约 4 KB，
> 上传过程要一块 2100 B 的连续内存 ⇒ `Exception 29` 重启。浏览器打开它只会看到一个登录框，
> 提交镜像 = 设备白重启一次。**它只在安全模式下可用**（那里空闲堆 17.3 KB，实测 584 KB
> 镜像 12 秒过一次），安全模式页上有「打开固件上传页」按钮，不用记地址。
> 日常刷固件走命令行 ArduinoOTA：`pio run -t upload --upload-port <设备IP>`
> —— 完整流程与判据见 `flashing-and-ota.md` §6。

> **安全模式下的路由是另一组**：`/`（安全模式页）、`/safe-status`、`/exit`、`/update`。
> 它们只在设备进安全模式时存在；业务模式下访问 `/safe-status`、`/exit` 是 **404**。

> `web/` 就是三个页面的源文件（`index.html` 面板、`setup.html` 配网页、`safe.html` 安全模式页），
> `tools/gen_web_assets.py` 在构建时把 `web/theme.css` 展开、压缩内联，
> **改 `web/` = 改设备页面**，只有一份前端。

---

## 2. `GET /api/status` —— 设备状态（排障第一站）

无参数。响应 `data` 字段：

| 字段 | 类型 | 说明 |
|---|---|---|
| `status` | string | `ap` / `running`（STA 已连） / `offline` |
| `ruleCount` | int | 当前映射规则条数（上限 20） |
| `forwardingEnabled` | bool | **转发总开关**（持久化，关掉就不再转发） |
| `transmitting` | bool | 此刻是否正在发射红外 —— 与上一个是两回事，别混用 |
| `firmware` | string | 固件版本，如 `1.3.14` |
| `sketchMD5` | string | 固件指纹，**OTA 后确认"到底刷没刷进去"用它**（等于 bin 的 MD5） |
| `ota` / `otaPort` | bool / int | OTA 可用性与端口（8266） |
| `uptime` | int | 开机秒数。**判"有没有重启"只看它** |
| `freeHeap` | int | 空闲堆（字节） |
| `maxFreeBlock` | int | 最大连续块。**单看 freeHeap 会漏掉"总量够但拿不出整块"** |
| `fragmentation` | float | 碎片率（%）。压测后会升高，**几分钟内自己回落，不是泄漏** |
| `heapUsage` | float | 仅作参考 —— 分母是启动早期的估算值，别拿它当判据 |
| `resetReason` / `resetReasonText` | int / string | 上次重启原因（SDK 值）：0 上电 / 1 硬件 WDT / **2 异常（崩溃）** / 3 软件 WDT / 4 软件重启（含 OTA）/ 5 深睡 / 6 外部复位 |
| `lowMemRestarts` | int | 因内存见底主动重启的次数（连续 3 次后不再重启，只保命） |
| `wifiConfigured` / `wifiMode` / `wifiConnected` / `wifiSSID` | — | 配网状态；`wifiMode` = `AP` / `STA` / `NONE` |
| `localIP` / `rssi` | — | STA 连上才有 `rssi` |
| `mqttConfigured` / `mqttConnected` / `mqttServer` | — | MQTT 状态 |
| `mqttPort` / `mqttUsername` | int / string | 供配网页预填 —— 不预填的话，"只改服务器"会把用户名一起清掉 |
| `wifiPasswordSet` / `mqttPasswordSet` | bool | **只报"存过没有"，不报密码本身**；配网页据此把提示写成"留空 = 不改动"还是"请填密码" |

```bash
curl -s --noproxy '*' http://192.168.11.111/api/status
```

---

## 3. 映射规则 `GET` / `PUT /api/maps`

### GET —— 查询全部规则

大响应走 chunked 流式发送（不整份进内存）。`data` 是规则数组：

```json
{
  "id": 3,
  "name": "音量+",
  "aSignal": { "useTiming": false, "protocol": 1, "data": "0x11B08F7",
               "repeatCount": 0, "timingCount": 0, "timingBase64": "" },
  "bSequence": {
    "signals": [ { "useTiming": true, "protocol": 0, "data": "0x00000000",
                   "repeatCount": 1, "timingCount": 68, "timingBase64": "…" } ],
    "intervals": [40]
  }
}
```

- `useTiming:false` = 协议信号，看 `protocol` + `data`（十六进制字符串）；
  `useTiming:true` = 时序码，看 `timingBase64` + `timingCount`。
- `data` 在 GET 里是**字符串**（如 `"0x11B08F7"`）；写入时数字或字符串都接受。

### PUT —— 更新规则

两种形态，**批量是推荐形态**（单个更新是为兼容旧客户端保留的）：

```bash
# 批量：先清空全部规则，再按数组逐条添加（上限 MAX_MAP_RULES = 20）
curl -s --noproxy '*' -X PUT http://192.168.11.111/api/maps \
  -H 'Content-Type: application/json' \
  -d '{"rules":[{"id":1,"name":"音量+","aSignal":{...},"bSequence":{...}}]}'

# 单个更新（旧格式）
curl -s --noproxy '*' -X PUT http://192.168.11.111/api/maps \
  -d '{"id":1,"rule":{"name":"音量+","aSignal":{...},"bSequence":{...}}}'
```

成功：`data` = `"Rules updated, count: N"`（N = 实际写入条数，**少于提交条数说明有规则被拒**，
常见原因是超过 20 条或信号格式非法）。

> ⚠️ 批量更新会**先删光再写入**，提交前先 GET 一份存本地。前端在 ≥18 条时会提示接近上限。

#### 可选 `mode`（v1.3.16）—— 给程序化 / HA 客户端用

旧批量语义是「先删光再写」，HA 这类客户端为了删一条得把全量重发（漏一条就误删）。
现在可带 `mode`：

```bash
# append：只追加，不动已有规则（重复 A 信号 / 满 20 条 / 格式非法 计入 skipped）
curl -s --noproxy '*' -X PUT http://192.168.11.111/api/maps \
  -H 'Content-Type: application/json' \
  -d '{"mode":"append","rules":[{"name":"音量+","a":"1,0x11B08F7","b":{...}}]}'
# → data: "Rules appended, added: 1, skipped: 0"

# delete：按 id 删一条（或 ids 数组删多条）
curl -s --noproxy '*' -X PUT http://192.168.11.111/api/maps \
  -H 'Content-Type: application/json' -d '{"mode":"delete","id":3}'
# → data: "Rules deleted: 1, not found: 0"（一条都没删掉时 ⇒ 走 400）
```

**不带 `mode` 时行为与旧版逐字相同**（批量替换 / 单个更新），前端不受影响。
`mode` 只认 `append` / `delete`，其它值按默认语义走。

#### 落盘与 ID（v1.3.18）

- **一次请求只写一次 flash。** 旧版是逐条落盘 —— 面板"保存"走全量替换（先逐条删再逐条加），
  5 条规则 = **10 次全量文件重写**，实测 `total_time = 1.937 s`。现在循环内只改内存、
  结束后统一落盘一次，同一次请求降到 **0.2 s 量级**。没有实际增删时干脆不写。
- **落盘失败会明确报出来。** 旧版 `addRule` / `deleteRule` 丢弃了写入结果，文件没写成也回
  `success`，要等重启才发现规则没了。现在失败时 `data` 末尾带 `, flash write failed`
  且 HTTP 返回 **400**（内存里已生效，但没持久化）。
- **规则 ID 不再可能为 0。** `id` 在 1..255 之间分配、并避开已占用的号（旧版是裸的 `uint8_t++`，
  加到 255 回绕成 0，而 0 是非法值 ⇒ 那条规则**永远删不掉**）。
  ⚠️ 但 **`id` 仍会在每次全量替换（面板保存）后重新分配** —— 它只是本次的寻址句柄，
  别在 HA 自动化里长期按 `id` 引用某条规则；**规则的身份以 A 信号为准**。

---

## 4. 转发总开关 `PUT` / `POST /api/forwarding`

```bash
curl -s --noproxy '*' -X POST http://192.168.11.111/api/forwarding \
  -d '{"enabled":false}'
```

**落盘持久化**，重启后保持。关掉后 `IRForwarder::forwardSignal` 直接返回 false
（转发记录里会留下 `result:3` 转发关闭）。

---

## 5. 转发记录与试跑（"为什么没转发"的排查口）

### `GET /api/forward/records` —— 最近 8 条结论

| 字段 | 说明 |
|---|---|
| `ms` / `ageSec` | 设备开机毫秒 / 距今秒数 |
| `aCode` / `aProtocol` | A 侧信号（`aProtocol=255` 表示时序码） |
| `ruleId` | 命中的规则 id，**`-1` = 没匹配到任何规则** |
| `bCount` / `durationMs` | 发射的 B 信号数 / 耗时 |
| `result` | `0` 已发送 / `1` 未匹配 / `2` 发射失败 / `3` 转发关闭 |

### `DELETE /api/forward/records` —— 清空

### `POST /api/forward/test` —— 试跑一条 A 信号

**不看总开关**，立刻匹配并发射，把结论直接说清楚。

```bash
curl -s --noproxy '*' -X POST http://192.168.11.111/api/forward/test \
  -d '{"a":"1,0x11B08F7"}'        # 也接受纯字符串 "1,0x11B08F7"
```

`data` 是中文结论：`已匹配规则并发出` / `没有任何规则匹配这个信号` /
`匹配到了规则，但发射失败` / `转发总开关已关闭`。

> **自测请用音量信号，不要用"电源"**（会真的把电器打开）。
> 音量+ = `1,0x11B08F7`，音量− = `1,0x11B8877`。
> 发射头与接收头在同一块板上 ⇒ 设备会收到自己发出的 B 码，
> 记录里出现几条 `ruleId:-1 / result:1` 的自收噪声属正常。

---

## 6. 学习遥控器 `POST|GET /api/learn/*`

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/learn/start` | 启动学习 |
| GET | `/api/learn/status` | 轮询进度 |
| POST | `/api/learn/stop` | 中止 |
| GET | `/api/learn/result?format=` | 取结果 |

```bash
# 1) 启动：命令名用逗号分隔，最多 8 个；超时 5~120 秒，默认 30
curl -s --noproxy '*' -X POST http://192.168.11.111/api/learn/start \
  -d '{"deviceName":"空调遥控器","commandNames":"开机,关机,温度+","timeoutSeconds":30}'

# 2) 轮询：progress 变化后，对着设备按对应的键
curl -s --noproxy '*' "http://192.168.11.111/api/learn/status"
# {"success":true,"data":{"isLearning":true,"progress":"2/5","currentCommand":"温度+"}}

# 3) 取结果（format 省略 / string / base64 → SmartIR 字符串格式；json → 结构化）
curl -s --noproxy '*' "http://192.168.11.111/api/learn/result?format=json"
```

- 没有学习结果时 `/api/learn/result` 返回 **404**。
- 返回值结构与 SmartIR 配置文件一致（`manufacturer` / `supportedModels` /
  `commandsEncoding` / `supportedController` / `commands`），可直接喂给 SmartIR。

---

## 7. 信号 `GET /api/signals/recent`、`POST /api/signals/send`

```bash
# 增量拉取：只回 timestamp > since 的条目，无新信号时响应极小（约 64 B）
curl -s --noproxy '*' "http://192.168.11.111/api/signals/recent?since=123456"

# 发射：JSON 或纯字符串都行，支持序列 "1,0xFD48B7@40;1,0x00FD48B7"
curl -s --noproxy '*' -X POST http://192.168.11.111/api/signals/send \
  -d '{"signal":"1,0x11B08F7"}'
```

`data` 数组的每一项是 `SignalSequence`（`signals[]` + `intervals[]`，见 `signal-format.md`）。
最多保留最近 5 条（`MAX_RECENT_SIGNALS`）。

---

## 8. 设备 / 按钮配置 `GET /api/devices` + `POST /api/devices/chunk`（v1.3.10 起；v1.3.42 改分片写）

这份配置以前只存在浏览器 `localStorage`，换手机或清缓存就丢；现在真正存在固件的
`/devices.json`。

```bash
curl -s --noproxy '*' http://192.168.11.111/api/devices            # 没存过时回 []
```

⭐ **读是流式的**（v1.3.42）：响应走 chunked、每次只读文件的一小块 ⇒ 内存占用与配置大小
无关，几 KB 的配置也读得回来（旧版把整文件读进 String，峰值约 2×文件大小，配置一大就读不动）。

### ⭐⭐ 写必须用**分片**（v1.3.42）—— 这是踩过坑的结论

整份配置一次 POST **在设备堆紧时必然失败**，而且失败形态极难查：

- `ESP8266WebServer` 收 body 要**一次读完**（`Parsing-impl.h:160-169`），读不满就
  `CLIENT_MUST_STOP` ⇒ **连一个 4xx 都回不了**，浏览器只看到"网络错误"。
- 实测（111，刚学完 2 条时序码、最大连续块只剩 4528）：1677 B 的配置**必然收不下**。
  而"学完就把新按钮加进设备"恰恰总发生在这个时刻。
- ⛔ **加内存闸救不了**：闸在 handler 里，而失败发生在更早的"收 body"阶段 —— 永远慢一步。

**协议**：把配置按**字节**切成 ≤512 B 的片，逐片提交。

```bash
# 逐片串行发（body 是原始字节；⛔ 不要按 JS 的字符切，会把 emoji 的代理对切坏）
curl -s --noproxy '*' -X POST \
  "http://192.168.11.111/api/devices/chunk?seq=0&total=2&len=875" \
  -H 'Content-Type: application/octet-stream' --data-binary @chunk0.bin
```

| 参数 | 含义 |
|---|---|
| `seq` / `total` | 本片序号（从 0 起）/ 总片数 |
| `len` | **总字节数** —— 设备端在末片拿它和文件实际大小对账，能发现"少发了一片" |

- `seq=0` ⇒ 丢弃上次的半截临时文件后新建（**中断的上传不会永久占 flash**），并校验首字节是 `[`
- `seq=total-1` ⇒ 追加后校验「文件大小 == `len`」且末字节是 `]`，通过则原子 rename 成 `/devices.json`
- 中间片响应带 `data.received`（累计字节）
- 任一片失败都要**整份重来**（从 `seq=0`），⛔ 不能断点续 —— 那会写出语法上看不出来的坏配置

| 失败码 | 含义 | 该怎么办 |
|---|---|---|
| **409** `Upload session lost` | 中间片找不到临时文件（上次中断/被清） | **从 seq=0 重来**（前端已自动重试一次） |
| 413 `分片过大` / `设备配置过大` | 单片 >512 B / 累计 > `MAX_DEVICES_FILE_SIZE` | 前者是编程错误；后者提示用户删按钮 |
| 400 `Length mismatch` / `Expected a JSON array` | 总长不符 / 首末字节不是 `[` `]` | 编程错误 |
| 507 | 堆不够写这一片 | 稍后重试 |

### 整份 POST（`POST /api/devices`）仍保留，但只是**小配置快路径**

上限 `DEVICES_SINGLE_SHOT_MAX = 1024` 字节（超过即 **413**）。它的存在只为兼容外部脚本。
⚠️ 别以为 `MAX_DEVICES_FILE_SIZE` 是 8192 就能整份发 8 KB —— 那条路要框架一次读完 body。
**前端已全部走分片。**

- 固件**不解析 JSON**（分片路径连单片都不解析，只看首/末字节），只做原文存取 ——
  空闲堆只有几 KB，解析几 KB 配置会把堆抽干。
- 上限：`MAX_DEVICES_FILE_SIZE = 8192`（分片）/ `DEVICES_SINGLE_SHOT_MAX = 1024`（整份）。
- 前端策略是"以设备为准，localStorage 只当缓存"；**设备为空而本地非空时会把老配置推上去**，
  所以升级固件不会丢配置。

---

## 9. 日志 `GET /api/logs` —— 页面上没有入口的那一个

环形缓冲，最多 `MAX_LOG_ENTRIES = 10` 条。**面板上没有按钮，这是唯一一个"只能手敲地址"的接口** ——
里面记的是给排障看的东西（设备启动、读取文件、学习开始 / 中止），日常用不上，所以没在界面上占位置。

```bash
curl -s --noproxy '*' http://192.168.11.111/api/logs
```

```json
{"success":true,"status":"success","data":[
  {"timestamp":12336,"type":"system","content":"设备启动"},
  {"timestamp":12359,"type":"system","content":"读取Maps文件，大小: 1704 bytes"},
  {"timestamp":18725,"type":"system","content":"系统初始化完成"}
],"timestamp":8186169}
```

- `type`：`system` / `learn` / `receive` / `transmit` / `error`；`content` 最长
  `MAX_LOG_CONTENT_LEN = 64` 字符，超了截断。
- `timestamp` 是 **`millis()` 毫秒**（相对上电），不是 Unix 时间。想知道"多久以前"，
  拿 `/api/status` 的 `uptime`（秒）换算，别跟电脑时间比。
- **只记关键事件，不记接收到的信号** —— 那个走 `/api/signals/recent`（见 §7）。
- **会落盘**：内存里满了覆盖最旧一条；攒够 `MAX_LOG_ENTRIES × 2/3 = 6` 条（或内存吃紧时）
  把整份写进 `/ir_logs.json`，开机时 `Logger::init()` 再读回来 ⇒ **重启后前面那几条往往还在**
  （这也是为什么日志开头总有"设备启动"）。
- ⚠️ **没有清空接口**（这条路径只有 `GET`）。想清掉只能删设备上的 `/ir_logs.json`，
  或等环形缓冲被新记录自然挤掉。
- Home Assistant 侧的等价物是 `{"action":"get_logs"}`（见 §11.2）；
  HA 里那个「最新日志」传感器由设备每次写日志时主动推送，不用轮询。

---

## 10. 配网 `GET /setup`、`POST /save`、`GET /api/wifi/scan`、`POST /api/restore`

### 10.1 配网页

`GET /setup`（旧路径 `/config` 同页）返回**编进固件的 gzip 资源**（`web/setup.html`），
与面板共用一套配色变量（`web/theme.css`），深浅色自动跟随。页面里的动态内容
（当前 WiFi / IP / MQTT / 固件版本）由页面脚本读 `GET /api/status` 填，静态部分一次请求拿全。
表单是原生 `action="/save" method="POST"`，**脚本挂了也能配网**，脚本只做增强：

- 「搜索」按钮 → `GET /api/wifi/scan`，点一条即填入 SSID；
- 提交前把关：换了 SSID 又留空密码时先 `confirm()` 一次；
- 「回退到上次能联网的配置」→ `POST /api/restore`。

### 10.2 `GET /api/wifi/scan` —— 搜索周边 WiFi

**只在 AP 模式（设备正等着配网）下可用**，其它状态一律 `409`。
原因：设备联网时扫描毫无意义，而扫描要临时切到"热点+站点"双模并阻塞主循环 2~4 秒
（红外接收跟着停摆）；不联网又不在 AP 模式的设备根本访问不到 Web 服务。

```json
{"success":true,"data":{"networks":[
  {"ssid":"PAC","rssi":-48,"encrypted":true},
  {"ssid":"Guest","rssi":-78,"encrypted":false}
]}}
```

- 最多回 **12** 条，按信号强度从强到弱（SDK 的顺序），隐藏 SSID 跳过（手输即可）。
- 单天线设备：扫描期间**连在设备热点上的手机可能瞬断几秒**，属正常现象，重连后重试即可。
- 调用期间热点参数会被重申一次（切模式可能重置 AP 配置），避免把配网打断。

### 10.3 `POST /api/restore` —— 回退到「上次能联网的配置」

等价于串口 `AT+RESTORE`，手机端的自救通道。无 body。

- 404：没有备份（设备还没成功联过网）。
- 200：`{"success":true,"data":{"ssid":"PAC","restartIn":2}}`，随后**约 2 秒后自动重启**
  （先把手里的响应发完再重启）。

### 10.4 `POST /save` 的语义（v1.3.14 起）

字段：`ssid`（必填）、`password`、`mqtt_server`、`mqtt_port`、`mqtt_username`、
`mqtt_password`、`mqtt_clear`（勾了=关闭 MQTT，取值任意）。

**整页一条规则：输入框留空 = 这一项保持原样。**

| 字段 | 留空时 |
|---|---|
| `ssid` | **报错不保存**（绝不存成"无配置"，否则会陷进回 AP 的死循环） |
| `password` | SSID 没变 ⇒ 沿用已保存的密码；SSID 变了 ⇒ 视为新网络没有密码（开放网络，页面上会先确认） |
| `mqtt_server` | 不动 MQTT（要关掉请用 `mqtt_clear`） |
| `mqtt_port` / `mqtt_username` / `mqtt_password` | 服务器没变 ⇒ 沿用已保存的值 |

保存成功后设备 LED 快闪 3 下，收尾页停留 3 秒再重启。

> ⚠️ **`/save` 依然是"WiFi + MQTT 一起写"** —— 2026-09-26 曾在这里栽过：
> 为验证 MQTT 通路走了 `/save`，用整片备份里过期的空密码回填，把 WiFi 顶掉、设备退进 AP 模式。
> 现在"留空即沿用"这条规则就是为它加的；只想改 MQTT 仍然可以走串口
> `AT+MQTT=<ip>,<port>[,<user>,<pass>]`（见 `flashing-and-ota.md`），完全不用碰 WiFi。

---

## 11. MQTT 接口

### 11.1 主题

| 方向 | 主题 | 说明 |
|---|---|---|
| 订阅 | `homeassistant/orvibo-ct30w/command` | 收命令 |
| 发布 | `homeassistant/orvibo-ct30w/state` | 命令响应 / 状态上报 |
| 发布 | `homeassistant/orvibo-ct30w/state/status` | `online` / `offline`（遗嘱，retain） |
| 发布 | `homeassistant/orvibo-ct30w/log` | 日志 |
| 订阅 | `smartir/climate/+/send`、`smartir/media_player/+/send`、`smartir/fan/+/send`、`smartir/light/+/send` | SmartIR 直接下发信号 |

连接参数：`clientId = ORVIBO-CT30W-<chipId>`；配了用户名才走鉴权；
遗嘱 = `state/status` 发 `offline`。重连退避 5→10→20→40→60 秒。

### 11.2 `command` 支持的 action

命令就是一段 JSON，`action` 与同名 HTTP 接口一一对应：

| action | 参数 | 对应 HTTP |
|---|---|---|
| `get_status` | — | `/api/status` |
| `query_maps` | — | `GET /api/maps` |
| `update_map` | `rules[]`，可选 `mode`（`append`/`delete`） | `PUT /api/maps` |
| `get_logs` | — | `GET /api/logs` |
| `start_learn` | `deviceName`, `commandNames`, `timeoutSeconds` | `/api/learn/start` |
| `query_learn_status` | — | `/api/learn/status` |
| `stop_learn` | — | `/api/learn/stop` |
| `query_learn_result` | `format` | `/api/learn/result` |
| `get_recent_signals` | `since` | `/api/signals/recent` |
| `send_signal` | `signal` | `/api/signals/send` |
| `get_forward_records` | — | `GET /api/forward/records` |
| `forward_test` | `a` | `POST /api/forward/test` |
| `set_forwarding_enabled` | `enabled` | `/api/forwarding` |

```json
{"action":"send_signal","signal":"1,0x11B08F7"}
{"action":"get_status"}
```

响应发到 `state`，信封与 HTTP 一致（`success` / `status` / `data` / `timestamp`）；
`data` 恒为对象或数组，且**总带 `forwardingEnabled`** 字段（不是对象时塞进 `data.result`）。
`state` 单条消息上限 = MQTT 缓冲 8,192 字节，超了就不发（日志里会记）。

### 11.3 SmartIR

`smartir/<domain>/<entity>/send` 上的 payload 是**裸信号字符串**，不要 JSON 包装
（Base64 / 协议码 / 序列字符串都收）。格式细节与配置示例见
`architecture/smartir-mqtt.md`。

### 11.4 Home Assistant 自动发现

连上 broker 后自动发布 Discovery（前缀 `homeassistant/`），HA 里出现：

| 实体 | 主题 |
|---|---|
| binary_sensor 在线状态 | `homeassistant/binary_sensor/<id>/status/config` |
| sensor 设备状态 | `homeassistant/sensor/<id>/state/config` |
| sensor 最新日志 | `homeassistant/sensor/<id>/log/config` |
| switch 转发开关 | `homeassistant/switch/<id>/forwarding_enabled/config` |

Discovery 发布失败会按 5→10→20 秒重试，直到成功。

> **v1.3.17 起，Discovery 是"零堆分配"的**：4 条配置里只有 `device.identifiers` 含 chipId，
> 其余全是编译期常量，已改成 **PROGMEM 常量模板 + 逐字节拼进栈缓冲**（不再用
> `DynamicJsonDocument` + `serializeJson`）。所以它在**配了 MQTT、空闲堆只剩 ~4 KB** 时也能发出。
> 之前那句 `freeHeap ≥ 4096` 的门槛已删除（正是它让本机 Discovery 从未真正发布过）。
> 顺带修正：`device.sw_version` 现在报**真实固件版本**，不再硬编码 `1.0.0`。

### 11.5 一条已知行为

**1 KB 收发缓冲是"配了 MQTT 才分配"**（`MQTTClient::ensureBuffer()`）—— 没配 MQTT 时它不占堆，
v1.3.22 前是 8 KB（曾把空闲堆压到 3.6 KB），现改为 1024 B（`MQTT_BUFFER_SIZE`，`src/config/constants.h:196`）。但**清空 MQTT 后这 1 KB 不会归还**
（`PubSubClient::setBufferSize` 不能缩到 0），空闲堆会停在 ~5 KB，**重启才回到 ~12 KB**。
别误判成内存泄漏。

> ⚠️ 由此推出一条硬约束（v1.3.17）：**配了 MQTT 时空闲堆只剩 5 KB 上下**，
> 所以**任何"写死的堆门槛"都必须低于这个水位**，否则功能会静默失效。
> v1.3.16 就踩了：Discovery 的 `freeHeap ≥ 4096`、规则落盘的 `freeHeap ≥ 4096`、
> 日志的 `freeHeap ≥ 4096` 全部恒真 ⇒ 配了 MQTT 的设备**永远不发布 Discovery、
> 规则改动不落盘（重启就丢）、非 error 日志被全丢**。四处已在 v1.3.17 修正。
> 排障入口：`AT+QUERY` 直接回显当前空闲堆。

### 11.6 状态自动回推（v1.3.16）

`state` 主题上的设备状态**不只在收到 MQTT 命令时才更新**：设备每 5 秒比对一次状态，
发现变化就发布一次。所以用**手机面板**关掉转发开关、或在网页上增删规则后，
HA 里的 switch / sensor 会在 **5 秒内**自动跟上 —— 不用 HA 再发一次 `get_status`。

比对在设备端做，无变化时**一个字节都不发**（成本只是几次字段比较），不产生 MQTT 空转流量。
MQTT 断线重连后还会强制完整重发一次状态（覆盖"HA 自己重启后不知道当前值"的情况）。

> **v1.3.17 修正**：这段回推原先挂在"Discovery 已发布"之后，而 Discovery 有自己的
> `freeHeap ≥ 4096` 门槛；配了 MQTT 的设备空闲堆只有约 4.0 KB，恰好卡在门槛下方 ⇒
> Discovery 不发布、回推被一并挡死。现在两处都改了：**回推只要求 `MQTT 已连接`**（与 Discovery 解耦），
> Discovery 本身也改成零堆分配。同轮还修了「连上 broker 却听不到命令」——订阅失败（内存软门槛）
> 以前不会重试，现在每 2 秒重试直到订上。

---

## 12. 错误码

| 码 | 出现处 | 含义 |
|---|---|---|
| 400 | 各处 | 缺 body / JSON 非法 / 字段缺失（如 `/api/devices` 不是数组） |
| 403 | **所有接口** | **Host 头不在白名单** —— 用设备 IP 或 `xxx.local` 访问正常；用"解析到设备 IP 的伪造域名"访问会被挡（防 DNS rebinding，见 §0） |
| 404 | `/api/learn/result`、`/api/restore` | 还没有学习结果 / 还没有可回退的配置备份；`/safe-status`、`/exit` 在业务模式下也是 404 |
| 409 | `/api/wifi/scan` | 设备已联网 —— 搜索周边 WiFi 只在 AP 配网模式下有意义 |
| 409 | `POST /api/devices/chunk` | **分片上传会话丢了**（中间片找不到临时文件）⇒ 从 `seq=0` 整份重来（见 §8） |
| 413 | `POST /api/devices` | 超过 `DEVICES_SINGLE_SHOT_MAX = 1024` 字节（请改用分片） |
| 413 | `POST /api/devices/chunk` | 单片 > `DEVICES_CHUNK_MAX_BYTES = 512`，或累计 > `MAX_DEVICES_FILE_SIZE = 8192` |
| 500 | 各处 | 服务未初始化 / 保存失败 / 发射失败 |
| 503 | 任意 API 前 | 内存不足，稍后重试 |
| 507 | `POST /api/devices*` | 堆不够写这一份 / 这一片 |
| **000** | ~~`POST /api/devices`~~ | ~~请求体收不下时框架直接丢连接~~ —— **v1.3.42 起不再出现**：分片后每片只占 ~1.5 KB 连续块，整份路径也有 1024 上限兜住（实测堆最紧时 ≤1024 全部 200、>1024 全部明确 413） |

---

## 13. 一排就通的冒烟脚本

```bash
B=http://192.168.11.111
curl -s --noproxy '*' "$B/api/status"                              # 活着 + 版本
curl -s --compressed -o /dev/null -w "%{http_code}\n" "$B/"        # 面板 200
curl -s --compressed -o /dev/null -w "%{http_code}\n" "$B/setup"   # 配网页 200（同样必须 --compressed）
curl -s --noproxy '*' "$B/api/maps" | head -c 400                  # 规则
curl -s --noproxy '*' -X POST "$B/api/forward/test" \
  -d '{"a":"1,0x11B08F7"}'                                         # 端到端转发（音量信号！）
curl -s --noproxy '*' "$B/api/forward/records"                     # 看结论
```

没有 broker 也想验 MQTT 通路：`tools/mqtt_test_broker.py`（零依赖，见
`flashing-and-ota.md`）。

---

## 14. `GET` / `DELETE /api/diag/freeze` —— 网络僵死的现场（v1.3.36 起）

设备陷入"僵死态"（主循环在跑、802.11 自认在线、但 TCP/HTTP 全死、不重启也不自愈）时，
`NetWatchdog` 会在**重启之前**把现场写进 flash 的 `/freeze.json`，靠这个端点取回。
完整说明见 `docs/v1.3.36-38-changes.md` 与 `docs/troubleshooting.md`。

```bash
B=http://192.168.11.111
curl -s --noproxy '*' "$B/api/diag/freeze"
curl -s --noproxy '*' -X DELETE "$B/api/diag/freeze"
```

**GET 响应**：

```jsonc
{
  "success": true, "status": "success",
  "data": {
    "hasSnapshot": true,          // flash 里有没有现场
    "netRestartCount": 1,         // 连续因网络僵死重启的次数（上限 3）
    "restartedByNetwork": true,   // 本次启动是不是自愈触发的
    "freeze": {                   // 没存过则是 {}
      "uptime": 4821,             // 秒
      "resetReason": 6, "resetReasonText": "External System",
      "wifiStatus": 3, "rssi": -49,
      "ip": "192.168.11.111", "gateway": "192.168.11.1", "ssid": "PAC",
      "freeHeap": 6088, "maxFreeBlock": 3320, "fragmentation": 45,
      "frozeForMs": 90120,        // 从"最后一次探测成功"到"判死"过了多久
      "lastIrAgoMs": 12500,       // 距上次收到红外
      "lastHttpAgoMs": 90300,     // 距上次应答 HTTP
      "netRestarts": 1,
      "probes": [                 // 最近 8 次探测，每项 = [时刻(秒), 耗时ms, 结论, 连上没]
        [4700, 12, 0, 1],
        [4770, 150, 1, 0]
      ]
    }
  }
}
```

`probes[]` 每项四个数：`[探测时刻(uptime 秒), 耗时 ms, 结论, connected]`。
结论：`0` = 正常、`1` = 慢（≥112 ms）、`2` = 本轮跳过（刚收到过红外，避让）。
`connected` **不参与判定**，只用来事后区分"慢 + 连上了"与"慢 + 被拒"。

**DELETE** 返回 200 并清掉 `/freeze.json`。

> ⭐ **取证完一定要 `DELETE`**：不清的话下次分不清是新现场还是旧的，
> `/api/status` 的 `hasFreezeSnapshot` 也会一直挂着 `true`。

### `/api/status` 里同名的三个字段

| 字段 | 含义 |
|---|---|
| `netRestartCount` | 连续因网络僵死重启的次数（稳定跑满 5 分钟会清零） |
| `netRestartedByNetwork` | **本次**启动是不是自愈触发的（用来一眼分清"被自愈了"还是"用户刷的"） |
| `hasFreezeSnapshot` | flash 里有没有待取的现场 |
