(function () {
  'use strict';

  const STORAGE_IP = 'ir_device_ip';
  const STORAGE_DEVICES = 'ir_devices';
  const STORAGE_LAST_DEVICE = 'ir_last_device_id';
  const STORAGE_DEVICES_META = 'ir_devices_meta';   // { v, seen, hash, at }
  const STORAGE_LEARN_DEVICE = 'ir_learn_device';   // 上次学习的设备名，用于「保存到设备」预填

  const PROTOCOL_NAMES = { 1: 'NEC', 2: 'RC5', 3: 'RC6', 4: 'Sony', 5: 'Samsung' };

  // 转发记录的四种结论（与固件里的 ForwardResult 一一对应）
  const FORWARD_RESULT_TEXT = {
    0: { text: '已发出', cls: 'fwd-ok' },
    1: { text: '未匹配规则', cls: 'fwd-warn' },
    2: { text: '发射失败', cls: 'fwd-err' },
    3: { text: '转发已关闭', cls: 'fwd-warn' }
  };

  const state = {
    tab: 'status',
    status: null,
    uptime: null,        // 设备上电秒数，用来把"上电毫秒"换算成"多久以前"（见 formatTime）
    uptimeAtMs: 0,       // 取到上面这个 uptime 时的**本机**时刻，用来把它外推到现在
    statusFails: 0,
    reconnectTimer: null,
    reconnectDelay: 0,       // 重连间隔（指数退避），连上后归零
    recent: [],
    recentSig: null,         // 上次渲染出来的列表指纹；一样就不重建 DOM
    recentExpanded: {},      // key -> true，展开状态记在这里，重建后不丢
    liveRecent: false,
    recentPollFails: 0,
    captureAbort: null,
    forwardRecords: [],
    learnStatus: null,
    learnResult: null,
    rules: [],
    ruleDetailId: null,
    ruleNewVisible: false,
    rulePreFillB: null,
    rulePreFillA: null,
    devices: [],
    devicePanelId: null,
    deviceAddVisible: false,
    devicesSync: 'checking',   // 配置在设备上还是只在本机：checking/ok/local/offline
    pendingSaveSignal: null
  };

  // 页面有两种打开方式：
  //   1) 直接在浏览器里访问设备地址（推荐）—— 此时 location 就是设备，免填 IP；
  //   2) 把 index.html 当本地文件打开 —— 只能靠用户手填的设备地址。
  function isServedFromDevice() {
    return location.protocol === 'http:' || location.protocol === 'https:';
  }

  function getOrigin() {
    if (isServedFromDevice()) return location.origin;
    const ip = localStorage.getItem(STORAGE_IP);
    if (!ip || !ip.trim()) return null;
    return 'http://' + ip.trim().replace(/^https?:\/\//, '');
  }

  function getBaseUrl() {
    const origin = getOrigin();
    return origin ? origin + '/api' : null;
  }

  function showToast(msg) {
    const el = document.getElementById('toast');
    el.textContent = msg;
    el.classList.remove('hidden');
    // 连续两条 toast 之间必须"摘掉再加"并强制回流，否则 CSS 动画只在第一次播放，
    // 第二条消息是硬切出来的（class 一直挂着，浏览器不会重放 animation）
    el.classList.remove('show');
    void el.offsetWidth;
    el.classList.add('show');
    clearTimeout(el._toastTimer);
    el._toastTimer = setTimeout(function () {
      el.classList.add('hidden');
      el.classList.remove('show');
    }, 2500);
  }

  function needIp() {
    if (!getBaseUrl()) {
      showToast('请先在设置中填写设备地址');
      return false;
    }
    return true;
  }

  // ── 固件报错翻译 ──────────────────────────────────────────────────────
  // 固件在不少路径上直接返回英文字面量（api_handlers / web_server 里就是英文），
  // 以前这些字符串原样弹给用户："Low Memory"、"Missing request body"、"Rule updated
  // in RAM but flash write failed"… 对一个只会按遥控器的人毫无意义。
  // 能对上就换成人话；对不上就照原样显示 —— 宁可保留信息，也不要瞎猜成"操作失败"。
  const FIRMWARE_ERROR_CN = {
    'Low Memory': '设备内存不足，请稍后再试（面板通常 30 秒内自己恢复）',
    'Insufficient memory': '设备内存不足，请稍后再试',
    // v1.3.44 起发射失败会**如实**返回（以前无论发没发都回 "Signal sent"）
    'Failed to send signal': '信号没能发出（设备内存不足），请过几秒再点一次',
    'Missing request body': '请求内容为空',
    'Empty payload': '请求内容为空',
    'Invalid JSON': '数据格式不对，设备没能解析',
    'Invalid rule format': '规则格式不对',
    "Invalid rule ID or missing 'rules' array": '规则编号无效，或缺少规则列表',
    "append mode requires 'rules' array": '新增规则时缺少 rules 列表',
    "delete mode requires 'id' or 'ids'": '删除规则时缺少要删的编号',
    'Failed to update rule': '规则保存失败',
    'Rule updated': '规则已保存',
    'Rule updated in RAM but flash write failed': '规则只改进了内存，写 Flash 失败 —— 断电会丢',
    'Failed to send signal': '红外发射失败',
    'Failed to get mapping rules': '读取规则失败',
    'Failed to parse response': '设备返回的内容无法解析',
    'Failed to convert learned result': '学习结果是有的，但设备没能把它转成文本（这是固件问题，不是你没学过）',
    'Device learn is not active': '设备当前没有在学习',
    'Device learn stopped': '学习已停止',
    "Missing 'deviceName' field": '缺少设备名',
    "Missing 'commandNames' field": '缺少指令名',
    "Missing 'rule' field": '缺少规则内容',
    "Missing 'enabled' field (true/false)": '缺少 enabled 字段（应为 true 或 false）',
    'No learned device found': '没有已学习的设备',
    'Not Found': '设备上没有这个接口（可能是旧版固件）',
    'Forbidden': '被设备拒绝了（来源地址不在白名单里）',
    'Service not initialized': '设备服务还没就绪，稍后再试',
    'Expected a JSON array': '设备期望一个数组'
  };

  function translateFirmwareError(msg) {
    if (!msg) return msg;
    const m = String(msg).trim();
    if (FIRMWARE_ERROR_CN[m]) return FIRMWARE_ERROR_CN[m];
    // 带变量的报文单独匹配（这些拼接结果对不上上面的常量表）
    if (m.indexOf('Body must be') === 0) return '请求体格式不对';
    var g = /^Invalid JSON:\s*(.*)$/.exec(m);
    if (g) return '数据格式不对，设备没能解析';
    g = /^Unknown action:\s*(.*)$/.exec(m);
    if (g) return '设备不认识这个动作：' + g[1];
    g = /^Rules appended, added:\s*(\d+), skipped:\s*(\d+)/.exec(m);
    if (g) {
      if (Number(g[1]) === 0) return '没有新增：A 信号可能和已有规则重复，或规则数已达上限';
      return '已新增 ' + g[1] + ' 条规则' + (m.indexOf('flash write failed') >= 0 ? '，但写 Flash 失败 —— 断电会丢' : '');
    }
    g = /^Rules deleted:\s*(\d+), not found:\s*(\d+)/.exec(m);
    if (g) return '已删除 ' + g[1] + ' 条规则' + (Number(g[2]) ? '（' + g[2] + ' 条没找到）' : '');
    g = /^Rules updated, count:\s*(\d+)/.exec(m);
    if (g) {
      var extra = '';
      var sk = /skipped:\s*(\d+)/.exec(m);
      if (sk) extra = '（有 ' + sk[1] + ' 条没进去，多半是这批里 A 信号重复）';
      if (m.indexOf('flash write failed') >= 0) extra += '，但写 Flash 失败 —— 断电会丢';
      return '已更新 ' + g[1] + ' 条规则' + extra;
    }
    g = /^invalid rule format \((\d+) of (\d+) parsed\)/.exec(m);
    if (g) return '这批 ' + g[2] + ' 条规则里只有 ' + g[1] + ' 条能解析，已整批拒绝（一条都没动）';
    g = /^rules array too large \(max (\d+)\)/.exec(m);
    if (g) return '规则条数超过上限 ' + g[1] + ' 条';
    return m;
  }

  function handleApiError(err, data) {
    if (data && data.error) showToast(translateFirmwareError(data.error));
    else showToast('请检查设备 IP 与网络');
  }

  // 请求失败的统一出口。
  // quiet=true 专给后台轮询：轮询失败只落横幅、不弹 toast。断连时 status(3s) 与
  // reconnect(4s) 两个定时器会**同时**跑 ⇒ 平均 1.7s 一次失败，每次都把 toast 的
  // 2500ms 计时器重置回去 ⇒ toast 永久卡在屏幕上不消失，说的还和红横幅是同一件事。
  // 设备"接受了连接却不回话"是常态（内存见底时就这个表现）。没有超时的话请求会永远挂着，
  // 单飞轮询就再也发不出下一个 ⇒ 界面静悄悄地死掉，用户只能刷新页面。
  var API_TIMEOUT_MS = 8000;

  function fetchJson(url, opt) {
    if (typeof AbortController !== 'function') return fetch(url, opt);   // 老浏览器就没这保护了
    var ctl = new AbortController();
    var t = setTimeout(function () { ctl.abort(); }, API_TIMEOUT_MS);
    return fetch(url, Object.assign({}, opt, { signal: ctl.signal }))
      .then(function (r) { clearTimeout(t); return r; },
            function (e) { clearTimeout(t); throw e; });
  }

  // ── 全局串行闸（2026-10-04 定案）────────────────────────────────────────
  // 设备是**单线程**的：ESP8266WebServer 一次只服务一条连接，多出来的只能排在 lwIP 里，
  // 而每个排队连接都占着 pcb + pbuf ⇒ 那一刻 `maxFreeBlock` 被压下去，于是重接口
  // （`/api/signals/recent`，门槛是 `checkMemory` 的 `maxBlock ≥ 估量+512`）直接
  // 503 "Insufficient memory"。
  // 实测（`tools/page_load_probe.py`，111 @ v1.3.47，两种形态除并发外别无差别）：
  //   三路并发（面板「最近信号」页签首屏那三条）→ 40 轮里 33 轮 recent 503，maxBlock 只剩 2704~3536
  //   同组改串行（**零间隔**）                   → 76 条请求 0 失败，maxBlock 稳在 4500~4850
  // ⇒ 服务器是串行的，客户端**必须**是串行的。这里做成**唯一出口**：apiGet / apiPost /
  //   apiPut 与清空记录那两个 DELETE 全排进同一条链，任何调用点（含以后新写的）都造不出并发。
  // ⚠️ 出队条件是「**响应体也读完了**」，不是「响应头回来了」—— 体没读完时浏览器会另开
  //    一条连接去发下一个请求，那正是要消掉的那个并发。
  var _reqChain = Promise.resolve();

  function enqueue(task) {
    var run = _reqChain.then(task);
    // 链本身不能被拒绝污染：上一条失败也必须让后面的请求继续发
    _reqChain = run.then(function () {}, function () {});
    return run;
  }

  /** 所有 HTTP 的唯一出口。返回 {res, data}；响应体不是 JSON 时按原行为抛错。 */
  function apiRaw(method, path, opt) {
    var base = getBaseUrl();
    if (!base) return Promise.reject(new Error('no ip'));
    return enqueue(function () {
      return fetchJson(base + path, Object.assign({ method: method }, opt || {}))
        .then(function (res) {
          return res.text().then(function (txt) {
            return { res: res, data: txt ? JSON.parse(txt) : null };
          });
        });
    });
  }

  function apiError(data, quiet, status) {
    if (!quiet) handleApiError(null, data);
    var e = new Error(data && data.error ? String(data.error) : 'request failed');
    // 打上标记：已经提示过一次了，别在 catch 里再补一条"请检查设备 IP 与网络"
    // （以前固件报 "Low Memory" 时会连弹两条，第二条还是错的口径）
    e.reported = true;
    // HTTP 状态码：调用方据此区分"可重试"（409 上传会话丢了 / 5xx）与
    // "重试也没用"（400 语法错 / 413 配置过大）—— 分片保存要用，见 postDevicesChunked
    if (status) e.status = status;
    return Promise.reject(e);
  }

  function apiNetworkError(err, quiet) {
    if (err && err.reported) return Promise.reject(err);      // 已提示过
    if (!quiet && err && err.message !== 'no ip' && !/^\{/.test(err.message || '')) {
      showToast('请检查设备 IP 与网络');
    }
    return Promise.reject(err);
  }

  function apiGet(path, opts) {
    const quiet = !!(opts && opts.quiet);
    return apiRaw('GET', path)
      .then(function (r) {
        // ⚠️ 原来这里没把 res.status 传下去 ⇒ GET 的失败拿不到状态码，
        //    调用方（如 loadLearnResult）就没法区分"503 内存不够（可重试）"与
        //    "404 没学过（重试也没用）"。补上，行为与 apiPost 一致。
        if (!r.res.ok) return apiError(r.data, quiet, r.res.status);
        if (r.data && r.data.success === false) return apiError(r.data, quiet, r.res.status);
        return r.data;
      })
      .catch(function (err) { return apiNetworkError(err, quiet); });
  }

  function apiPost(path, body, contentType, opts) {
    const quiet = !!(opts && opts.quiet);
    const isJson = contentType === 'application/json';
    // 原始字节体（分片提交用）：必须原样透传，⛔ 不能落进下面的 String 分支变成空体
    const isBytes = (typeof Uint8Array !== 'undefined') && (body instanceof Uint8Array);
    const headers = {};
    if (isJson) headers['Content-Type'] = 'application/json';
    else if (isBytes) headers['Content-Type'] = 'application/octet-stream';
    else if (typeof body === 'string') headers['Content-Type'] = 'text/plain;charset=utf-8';
    const opt = { headers: headers };
    if (body != null) opt.body = isJson ? JSON.stringify(body) : ((isBytes || typeof body === 'string') ? body : '');
    return apiRaw('POST', path, opt)
      .then(function (r) {
        if (!r.res.ok) return apiError(r.data, quiet, r.res.status);
        if (r.data && r.data.success === false) return apiError(r.data, quiet, r.res.status);
        return r.data;
      })
      .catch(function (err) { return apiNetworkError(err, quiet); });
  }

  function apiPut(path, body, opts) {
    const quiet = !!(opts && opts.quiet);
    return apiRaw('PUT', path, {
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    })
      .then(function (r) {
        if (!r.res.ok) return apiError(r.data, quiet);
        if (r.data && r.data.success === false) return apiError(r.data, quiet);
        return r.data;
      })
      .catch(function (err) { return apiNetworkError(err, quiet); });
  }

  /** 协议 data：统一为紧凑 0x 形式，避免 0x00FDB04F 与 0xFDB04F 来回分叉 */
  function normalizeProtocolDataHex(d) {
    if (d == null || d === '') return '0x0';
    if (typeof d === 'number') return '0x' + (d >>> 0).toString(16).toUpperCase();
    var s = String(d).trim();
    if (/^0x[0-9a-fA-F]+$/.test(s)) {
      var n = parseInt(s, 16);
      if (!isNaN(n)) return '0x' + (n >>> 0).toString(16).toUpperCase();
    }
    return s;
  }

  function oneSignalToStr(sig) {
    if (!sig) return '';
    if (sig.useTiming && sig.timingBase64) {
      const r = sig.repeatCount > 1 ? '|' + sig.repeatCount : '';
      return sig.timingBase64 + r;
    }
    if (sig.useTiming && sig.timingCount && sig.timingCount > 0) {
      return ''; // 无 raw 数组时无法序列化，后端通常会给 timingBase64
    }
    const p = sig.protocol != null ? sig.protocol : 0;
    const d = normalizeProtocolDataHex(sig.data);
    const r = sig.repeatCount > 1 ? ',' + sig.repeatCount : '';
    return p + ',' + d + r;
  }

  function sequenceToStr(seq) {
    if (!seq || !seq.signals || !seq.signals.length) return '';
    const signals = seq.signals;
    const intervals = seq.intervals || [];
    const n = signals.length;
    const parts = [];
    let firstInterval = null;
    if (intervals.length === n) {
      firstInterval = intervals[0];
      for (var i = 0; i < n; i++) {
        var seg = oneSignalToStr(signals[i]);
        if (!seg) continue;
        var interval = i > 0 ? intervals[i] : null;
        if (interval != null && interval > 0) seg += '@' + interval;
        parts.push(seg);
      }
    } else {
      for (var j = 0; j < n; j++) {
        var seg2 = oneSignalToStr(signals[j]);
        if (!seg2) continue;
        var interval2 = intervals[j];
        if (interval2 != null && interval2 > 0) seg2 += '@' + interval2;
        parts.push(seg2);
      }
    }
    var repeatCount = (signals[0] && signals[0].repeatCount) || 1;
    var body = firstInterval != null && firstInterval > 0 ? '@' + firstInterval + ';' + parts.join(';') : parts.join(';');
    return repeatCount > 1 ? body + '|' + repeatCount : body;
  }

  function aSignalToStr(a) {
    if (!a) return '';
    return oneSignalToStr(a);
  }

  function bSequenceToStr(b) {
    if (!b) return '';
    return sequenceToStr(b);
  }

  // ── 序列化"能不能完整还原"的显式判据 ──────────────────────────────────
  // oneSignalToStr() 在"纯时序信号但没带 timingBase64"时返回 ''，而 sequenceToStr()
  // 里 `if (!seg) continue;` 会把它**静默跳过** ⇒ 拼出来的 B 串短了一截。
  // 这个串一旦被写回设备（或用来新建规则），那条 B 就被换成残缺版 —— 用户什么都没改，
  // 数据却丢了，且全程没有任何提示。所以凡是"要发给设备"的路径都必须先过这道闸。
  function signalSerializable(sig) {
    if (!sig) return false;
    if (sig.useTiming && !sig.timingBase64 && sig.timingCount > 0) return false;
    return true;
  }

  function sequenceSerializable(seq) {
    if (!seq || !seq.signals || !seq.signals.length) return false;
    for (var i = 0; i < seq.signals.length; i++) {
      if (!signalSerializable(seq.signals[i])) return false;
    }
    return true;
  }

  // 能完整还原才返回字符串，否则返回 null（调用方必须拦住，不许提交）
  function sequenceToStrStrict(seq) {
    if (!seq || !seq.signals || !seq.signals.length) return null;
    if (!sequenceSerializable(seq)) return null;
    var s = sequenceToStr(seq);
    return s || null;
  }

  function signalSummary(str) {
    if (!str || !str.length) return '';
    if (str.indexOf(';') >= 0) return '多信号序列';
    if (/^\d+,0x[0-9a-fA-F]+/.test(str)) {
      var m = str.match(/^(\d+),0x([0-9a-fA-F]+)/);
      var name = PROTOCOL_NAMES[parseInt(m[1], 10)] || '协议' + m[1];
      return name + ' 0x' + m[2];
    }
    if (str.startsWith('[')) return '时序数组';
    if (/^[A-Za-z0-9+/=]+$/.test(str) && str.length % 4 === 0) return 'Base64 时序';
    return str.length > 20 ? str.slice(0, 20) + '…' : str;
  }

  function copyToClipboard(text) {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      return navigator.clipboard.writeText(text).then(function () {
        showToast('已复制');
      }).catch(function () {
        fallbackCopy(text);
      });
    }
    fallbackCopy(text);
  }

  function fallbackCopy(text) {
    var ta = document.createElement('textarea');
    ta.value = text;
    ta.style.position = 'fixed';
    ta.style.opacity = '0';
    document.body.appendChild(ta);
    ta.select();
    try {
      document.execCommand('copy');
      showToast('已复制');
    } catch (e) {
      showToast('请长按输入框粘贴');
    }
    document.body.removeChild(ta);
  }

  function formatTime(ts) {
    if (!ts) return '';
    // ⚠️ 固件给的是**上电毫秒数**（logger.cpp 里记的是 millis()），设备上没有 RTC。
    // 以前直接 `new Date(ts)` ⇒ 得到 1970-01-01，与"现在"相差 56 年，
    // 于是下面所有分支都不成立，最终落到 toLocaleDateString()
    // ⇒ **每条信号都显示 1970/1/1**，"刚刚 / N 分钟前"完全失效 ——
    // 而这恰恰是"最近信号"这一页唯一有价值的元信息（这条码是刚按的还是十分钟前的）。
    // 正确做法：用 /api/status 的 uptime（秒）把它换算成"多久以前"。
    var t = typeof ts === 'number' ? ts : parseInt(ts, 10);
    if (!isFinite(t)) return '';
    if (typeof state.uptime !== 'number') return '';   // 状态还没拿到，先不瞎显示
    // ⚠️ uptime 只在**状态页**轮询（3s）时刷新，而"最近信号"页会把状态轮询停掉
    //    ⇒ 直接拿 state.uptime 当"现在"，时间会**冻在**离开状态页的那一刻
    //    （待了 10 分钟仍显示"刚刚"）。所以用本机时钟把这段时间补上。
    var nowUp = state.uptime + (Date.now() - state.uptimeAtMs) / 1000;
    var age = nowUp - t / 1000;
    if (age < 0) age = 0;    // 设备重启过（计数归零）或两边没同步
    return formatAge(Math.floor(age));
  }

  function switchTab(tab) {
    state.tab = tab;
    document.querySelectorAll('.panel').forEach(function (p) {
      var on = (p.dataset.tab === tab);
      p.classList.toggle('active', on);
      // 非当前页的面板加 hidden 属性：读屏和键盘 Tab 就不会一路扫过全部 6 个面板。
      // （以前只是 CSS display:none，辅助设备无法区分"这是未选中的页签内容"。）
      p.hidden = !on;
    });
    document.querySelectorAll('.tab').forEach(function (t) {
      var on = (t.dataset.tab === tab);
      t.classList.toggle('active', on);
      // role="tab" 必须配 aria-selected，否则读屏会念成"标签页"却不说哪个选中
      t.setAttribute('aria-selected', on ? 'true' : 'false');
    });
    // 状态页 3 秒轮询；切到别的页就停，别让手机白白耗电
    if (tab === 'status') startStatusPoll(); else stopStatusPoll();
    if (tab === 'recent') {
      // ⚠️ state.uptime 只由**状态页**的轮询喂（renderStatus）。而 init() 会恢复上次
      //    停留的页签 —— 若上次停在"最近信号"，这里不补一次状态，uptime 就一直是 null
      //    ⇒ formatTime 全部返回空串 ⇒ 每条信号都**看不到时间**（用户实测报的）。
      //    静默拉取：失败只落横幅，不弹 toast。
      if (typeof state.uptime !== 'number') loadStatus(true);
      loadRecent();
      loadForwardRecords();
      if (state.liveRecent) startRecentPoll();   // 从别的页切回来，实时监视仍在开就继续
    } else {
      stopRecentPoll();
      stopCapture();
    }
    if (tab === 'learn') {
      if (state.learnStatus && state.learnStatus.isLearning) startLearnPoll();
      // 切回学习页也把上次学到的结果取回来（静默：没学过时设备返 404，不该弹提示）
      loadLearnResult(0, true);
    }
    if (tab === 'rules') loadRules();
    if (tab === 'devices') {
      loadDevices();
      // 设备页那条「刚学到的指令还没进设备」要用 state.learnResult 才能算出来
      // （静默：没学过时设备返 404，不该弹提示）
      loadLearnResult(0, true);
    }
  }

  // ==================== 状态页 ====================

  // ── 轮询器：递归 setTimeout + 单飞 ──────────────────────────────────────
  // 以前三个轮询全用 setInterval —— 它不等上一次请求回来就再发一次。而项目自己
  // （gen_web_assets.py:13-18）写着"设备扛不住并发"：保存一次规则实测 1.9s，
  // 此时 1.2s 的实时监视轮询会稳定叠出 2~3 个并发请求，把约 20KB 的堆按在地上。
  // 现在：① 上一次没回来就不发新的；② 回来的那一刻才开始算下一次的间隔。
  var pollers = {};

  function startPoller(name, fn, intervalMs) {
    stopPoller(name);
    var p = pollers[name] = { fn: fn, interval: intervalMs, timer: null, busy: false, stopped: false };
    function tick() {
      if (p.stopped) return;
      // 页面在后台 / 上一次请求还没回来 ⇒ 这一轮跳过，不叠加
      if (document.hidden || p.busy) {
        p.timer = setTimeout(tick, p.interval);
        return;
      }
      p.busy = true;
      Promise.resolve()
        .then(p.fn)
        .catch(function () {})
        .then(function () {
          p.busy = false;
          if (!p.stopped) p.timer = setTimeout(tick, p.interval);
        });
    }
    tick();
  }

  function stopPoller(name) {
    var p = pollers[name];
    if (!p) return;
    p.stopped = true;
    if (p.timer) clearTimeout(p.timer);
    delete pollers[name];
  }

  function startStatusPoll() {
    if (pollers.status) return;     // 已经在跑就别重启（stopReconnect 会再调一次）
    startPoller('status', function () { return loadStatus(true); }, 3000);
  }

  function stopStatusPoll() {
    stopPoller('status');
  }

  function loadStatus(quiet) {
    if (!needIp()) {
      setBanner('unreachable', '未配置设备地址');
      return Promise.resolve();
    }
    // quiet：轮询失败不弹 toast，只落横幅（详见 apiError 的注释）
    return apiGet('/status', { quiet: !!quiet }).then(function (data) {
      var d = data && data.data ? data.data : data;
      state.status = d;
      state.statusFails = 0;
      stopReconnect();
      renderStatus(d);   // 横幅由 renderStatus 按设备真实状态设置
    }).catch(function () {
      onUnreachable();
    });
  }

  // 连不上时的统一处理：提示 + 自动重连。
  // 关键：设备内存紧张时会自我关闭面板（固件侧最多 20 秒强制重开），
  // 所以这里绝不能是死胡同 —— 用户不该被要求"刷新页面"或"去检查电源"。
  function onUnreachable() {
    state.statusFails = (state.statusFails || 0) + 1;
    if (state.statusFails < 3) {
      setBanner('unreachable', '正在连接设备…');
    } else {
      setBanner('unreachable',
        '面板服务暂时无响应，正在自动重连…（设备内存紧张时会短暂关闭面板，通常 30 秒内自行恢复，无需刷新）');
    }
    startReconnect();
  }

  // 重连间隔：4s → 8s → 16s … 封顶 60s。
  // 设备正在自愈（内存紧张时会关面板、甚至自己重启）时密集重试只会把它按在地上 ——
  // 它只有约 20KB 堆，并发能力实测 4 路。
  var RECONNECT_MIN_MS = 4000;
  var RECONNECT_MAX_MS = 60000;

  function startReconnect() {
    // ⚠️ 以前这里不停状态轮询：status 3s + reconnect 4s 两条定时器一起打设备，
    // 平均 1.7s 一次请求，正是把"正在自愈的设备"拖死的最坏节奏。
    stopStatusPoll();
    if (state.reconnectTimer) return;
    scheduleReconnect();
  }

  function scheduleReconnect() {
    var d = state.reconnectDelay || RECONNECT_MIN_MS;
    state.reconnectDelay = Math.min(d * 2, RECONNECT_MAX_MS);
    state.reconnectTimer = setTimeout(function () {
      state.reconnectTimer = null;
      if (document.hidden) { scheduleReconnect(); return; }   // 后台不打扰设备
      loadStatus(true).then(reloadCurrentTab).catch(function () {});
      // 成功 ⇒ renderStatus → stopReconnect()；
      // 失败 ⇒ loadStatus → onUnreachable() → 这里再排下一轮（间隔已翻倍）
    }, d);
  }

  function stopReconnect() {
    if (state.reconnectTimer) {
      clearTimeout(state.reconnectTimer);
      state.reconnectTimer = null;
    }
    state.reconnectDelay = 0;
    // 重连成功后把状态轮询恢复回来（startReconnect 时为避免叠加关掉了）
    if (state.tab === 'status') startStatusPoll();
  }

  // 重连成功后，顺手把当前页签的数据也补回来（否则页面看着还是空的）
  function reloadCurrentTab() {
    if (state.tab === 'status') return;
    if (state.tab === 'recent') { loadRecent(true); loadForwardRecords(true); }
    else if (state.tab === 'rules') loadRules(true);
    else if (state.tab === 'devices') loadDevices(true);
  }

  function setBanner(kind, text) {
    var el = document.getElementById('status-banner');
    el.className = 'status-banner banner-' + kind;
    el.textContent = text;
  }

  // 配网模式下的「去配网」直达按钮：设备没上网时，面板能做的事本来就不多，
  // 把"下一步"直接摆在横幅下面，省掉"设置 → 配网入口"这两步。
  function setBannerAction(show) {
    document.getElementById('banner-action').classList.toggle('hidden', !show);
  }

  function formatUptime(sec) {
    if (sec == null) return '—';
    var d = Math.floor(sec / 86400);
    var h = Math.floor((sec % 86400) / 3600);
    var m = Math.floor((sec % 3600) / 60);
    if (d > 0) return d + ' 天 ' + h + ' 小时';
    if (h > 0) return h + ' 小时 ' + m + ' 分';
    if (m > 0) return m + ' 分 ' + (sec % 60) + ' 秒';
    return sec + ' 秒';
  }

  function rssiText(rssi) {
    if (rssi == null) return '—';
    var quality = rssi >= -60 ? '强' : (rssi >= -75 ? '中' : '弱');
    return rssi + ' dBm · ' + quality;
  }

  function formatBytes(n) {
    if (n == null) return '—';
    if (n >= 1024) return (n / 1024).toFixed(1) + ' KB';
    return n + ' B';
  }

  function renderStatus(d) {
    if (!d) return;

    // 设备没有 RTC，固件给的时间戳都是**上电毫秒数**（millis()）。
    // 记下当前的 uptime，前端才能把"上电毫秒"换算成"多久以前"
    // （见 formatTime —— 以前直接 new Date(ts)，于是每条信号都显示 1970/1/1）。
    if (typeof d.uptime === 'number') {
      state.uptime = d.uptime;
      state.uptimeAtMs = Date.now();   // 本机基准时刻，formatTime 靠它把 uptime 外推
    }

    // 顶部横幅：一眼看出设备在什么状态
    if (d.status === 'ap') {
      setBanner('ap', '配网模式：设备还没连上 WiFi');
      setBannerAction(true);
    } else if (d.status === 'offline') {
      setBanner('offline', '设备已上电但未连上 WiFi');
      setBannerAction(true);
    } else {
      setBanner('ok', '运行中');
      setBannerAction(false);
    }

    // 转发总开关（这才是"开没开转发"，不是"此刻有没有在发红外"）
    var btn = document.getElementById('btn-forward-toggle');
    var enabled = !!d.forwardingEnabled;
    btn.disabled = false;
    btn.textContent = enabled ? '已开启 · 点此关闭' : '已关闭 · 点此开启';
    btn.className = 'btn ' + (enabled ? 'btn-secondary' : 'btn-primary');
    document.getElementById('forward-box').classList.toggle('forward-off', !enabled);
    document.getElementById('forward-hint').textContent = enabled
      ? (d.transmitting ? '正在发射红外…' : '关闭后设备只收不发，规则照旧保留')
      : '转发已停：收到信号只记录，不会再发红外';

    // 明细表
    var rows = [
      ['WiFi', d.wifiConnected ? (d.wifiSSID || '已连接') + ' · ' + rssiText(d.rssi) : (d.wifiConfigured ? '未连接' : '未配置')],
      ['设备地址', d.localIP || '—'],
      ['MQTT / Home Assistant', d.mqttConnected ? '已连接 · ' + (d.mqttServer || '') : (d.mqttConfigured ? '未连接 · ' + (d.mqttServer || '') : '未配置')],
      ['转发规则', (d.ruleCount != null ? d.ruleCount : '—') + ' 条'],
      ['可用内存', formatBytes(d.freeHeap) + (d.heapUsage != null ? ' · 已用 ' + d.heapUsage.toFixed(1) + '%' : '')],
      ['已运行', formatUptime(d.uptime)]
    ];

    var grid = document.getElementById('status-grid');
    grid.innerHTML = '';
    rows.forEach(function (row) {
      var item = document.createElement('div');
      item.className = 'info-item';
      var k = document.createElement('span');
      k.className = 'info-key';
      k.textContent = row[0];
      var v = document.createElement('span');
      v.className = 'info-value';
      v.textContent = row[1];
      item.appendChild(k);
      item.appendChild(v);
      grid.appendChild(item);
    });

    document.getElementById('fw-version').textContent = d.firmware || '—';
    document.getElementById('fw-md5').textContent = d.sketchMD5 || '—';
  }

  function toggleForwarding() {
    if (!needIp()) return;
    var btn = document.getElementById('btn-forward-toggle');
    var next = !(state.status && state.status.forwardingEnabled);
    btn.disabled = true;
    apiPut('/forwarding', { enabled: next }).then(function (data) {
      var d = data && data.data ? data.data : data;
      if (state.status) state.status.forwardingEnabled = d.forwardingEnabled;
      showToast(d.forwardingEnabled ? '转发已开启' : '转发已关闭');
      loadStatus();
    }).catch(function () {
      showToast('设置失败');
    }).then(function () {
      btn.disabled = false;
      loadStatus();
    });
  }

  // ==================== 最近信号 ====================

  // incremental=true 时只问"比本地最新那条还新的"，无新信号设备只回一个 "[]"，开销极小
  function loadRecent(incremental, quiet) {
    if (!needIp()) return Promise.resolve();
    var url = '/signals/recent';
    var maxTs = 0;
    state.recent.forEach(function (it) { if (it.timestamp > maxTs) maxTs = it.timestamp; });
    if (incremental && maxTs > 0) url += '?since=' + maxTs;
    return apiGet(url, { quiet: !!quiet }).then(function (data) {
      var arr = data && data.data ? data.data : (Array.isArray(data) ? data : []);
      if (!incremental) {
        state.recent = arr;
        state.recentPollFails = 0;
      } else if (arr.length) {
        // 设备重启会让时间戳归零；这时丢弃本地列表，用设备端全量重建
        var oldest = arr[0].timestamp || 0;
        if (oldest < maxTs && maxTs - oldest > 60000) {
          state.recent = arr;
          // ⚠️ 顺带重取一次状态：uptime 基准还停在**重启前**那个大值上，
          //    而新信号的 timestamp 已经从 0 重新开始 ⇒ 不刷新的话每条都显示成
          //    "很多天前"。这里只补拉，不阻塞渲染；回来后再重画一遍时间。
          loadStatus(true).then(function () { renderRecent(true); });
        } else {
          state.recent = state.recent.concat(arr);
          var seen = {};
          state.recent = state.recent.filter(function (it) {
            var k = String(it.timestamp) + ':' + sequenceToStr(it);
            if (seen[k]) return false;
            seen[k] = 1;
            return true;
          });
          if (state.recent.length > 30) state.recent = state.recent.slice(-30);
        }
      }
      state.recentPollFails = 0;
      renderRecent();
    }).catch(function () {
      if (!incremental) {
        state.recent = [];
        renderRecent();
      } else {
        state.recentPollFails++;
        if (state.recentPollFails >= 3) {
          stopRecentPoll();
          state.liveRecent = false;
          showToast('设备响应超时，已暂停实时监视');
        }
      }
    });
  }

  function startRecentPoll() {
    startPoller('recent', function () { return loadRecent(true, true); }, 1200);
  }

  function stopRecentPoll() {
    stopPoller('recent');
  }

  function toggleLiveRecent() {
    state.liveRecent = !state.liveRecent;
    var btn = document.getElementById('btn-live-recent');
    if (state.liveRecent) {
      state.recentPollFails = 0;
      startRecentPoll();
      if (btn) { btn.classList.add('active'); btn.textContent = '停止监视'; }
      showToast('实时监视已开启');
    } else {
      stopRecentPoll();
      if (btn) { btn.classList.remove('active'); btn.textContent = '实时监视'; }
      showToast('实时监视已停止');
    }
  }

  // 一条信号的稳定标识。设备没有 RTC ⇒ timestamp 只是上电毫秒，
  // 配上序列内容足以区分（loadRecent() 去重也是这个判据）。
  function recentKey(item) {
    return String(item.timestamp) + ':' + sequenceToStr(item);
  }

  function findRecentByKey(key) {
    for (var i = 0; i < state.recent.length; i++) {
      if (recentKey(state.recent[i]) === key) return state.recent[i];
    }
    return null;
  }

  // 显示顺序：**最新的一条在最上面**。
  // 设备端是按 timestamp **升序**返回的（最旧在前，见 logger.cpp 的
  // getRecentSignalSequences），照它的顺序渲染就成了"越往下越新" —— 与所有人对
  // "最近信号"列表的预期相反（用户实测提出来问"上面的是更先还是更新"）。
  // ⚠️ state.recent 本身必须**保持升序**：loadRecent 的增量拉取拿 arr[0]（最旧那条）
  //    判断设备有没有重启过，把底层数组倒过来会让那个判据失效。
  function recentDisplayList() {
    return state.recent.slice().reverse();
  }

  function recentMetaText(item) {
    // 没匹配上规则的信号：标黄提示"按了但没人认"，这是最常见的排障入口
    // ⚠️ 判据是"没明确说匹配就是未匹配"（!== true），不是"明确说了没匹配才算"。
    // 用 === false 时，只要这条记录里压根没有 matched 字段（旧固件、或序列化时被省掉），
    // 就会显示成「已匹配」—— 而这页最核心的用途恰恰是靠"未匹配"高亮来找"按了没人认"。
    var when = formatTime(item.timestamp);
    var tag = (item.matched !== true ? '未匹配规则' : '已匹配');
    // uptime 还没拿到时 formatTime 返回空串 —— 别留下一个孤零零的" · "
    return when ? (when + ' · ' + tag) : tag;
  }

  // 只把"多久以前"这几行文本刷一遍，不动 DOM 结构。
  // 实时监视每 1.2s 拉一次，绝大多数拉回来是空的 —— 那就没必要重建整张表。
  function refreshRecentMeta() {
    var list = document.getElementById('recent-list');
    if (!list) return;
    var nodes = list.children;
    var shown = recentDisplayList();   // 必须与 renderRecent 用同一个顺序
    for (var i = 0; i < nodes.length && i < shown.length; i++) {
      var m = nodes[i].querySelector('.meta');
      if (m) m.textContent = recentMetaText(shown[i]);
    }
  }

  function renderRecent(force) {
    var list = document.getElementById('recent-list');
    var empty = document.getElementById('recent-empty');
    if (!list) return;
    bindRecentList(list);

    // ⚠️ 以前这里无条件 innerHTML='' 重建：实时监视 1.2s 一次 ⇒
    //   30 条 × 6 个按钮 ≈ 180 次监听绑定/轮，手机上肉眼可见地卡；
    //   更要命的是"展开/收起"只存在 DOM 的 class 里 ⇒ 刚点开看一条长码，
    //   1.2 秒后 DOM 被重建，它自己就收起来了，根本来不及读完。
    // 现在：列表内容没变就只刷时间文本；展开状态记在 state 里，重建后仍然展开。
    var sig = state.recent.map(recentKey).join('|');
    if (!force && sig === state.recentSig) { refreshRecentMeta(); return; }
    state.recentSig = sig;

    list.innerHTML = '';
    if (!state.recent.length) {
      empty.classList.remove('hidden');
      return;
    }
    empty.classList.add('hidden');

    recentDisplayList().forEach(function (item) {
      var key = recentKey(item);
      var str = sequenceToStr(item);
      var div = document.createElement('div');
      div.className = 'list-item' + (item.matched !== true ? ' unmatched' : '') +
                      (state.recentExpanded[key] ? ' expanded' : '');
      div.setAttribute('data-key', key);
      div.innerHTML =
        '<span class="meta"></span>' +
        '<span class="summary">' + escapeHtml(signalSummary(str)) + '</span>' +
        '<span class="signal-str" title="' + escapeAttr(str) + '">' + escapeHtml(str) + '</span>' +
        '<div class="actions">' +
        '<button type="button" class="btn btn-primary" data-act="send">发送</button>' +
        '<button type="button" class="btn btn-secondary" data-act="set-a">设为 A</button>' +
        '<button type="button" class="btn btn-secondary" data-act="set-b">设为 B</button>' +
        '<button type="button" class="btn btn-secondary" data-act="copy">复制</button>' +
        '<button type="button" class="btn btn-secondary" data-act="save">保存到设备</button>' +
        '<button type="button" class="btn btn-text" data-act="expand"></button>' +
        '</div>';
      list.appendChild(div);
    });
    refreshRecentMeta();
    refreshRecentExpandLabels();

    // 只保留还在列表里的展开状态，免得这个 map 跟着信号无限增长
    var live = {};
    state.recent.forEach(function (it) { live[recentKey(it)] = 1; });
    Object.keys(state.recentExpanded).forEach(function (k) {
      if (!live[k]) delete state.recentExpanded[k];
    });
  }

  function refreshRecentExpandLabels() {
    var nodes = document.getElementById('recent-list').children;
    for (var i = 0; i < nodes.length; i++) {
      var b = nodes[i].querySelector('[data-act="expand"]');
      if (b) b.textContent = nodes[i].classList.contains('expanded') ? '收起' : '展开';
    }
  }

  // 整张表只在 #recent-list 上挂一个 click，按 data-act 分派。
  // 以前每条重建都要重绑 6×N 个监听，现在与列表长度无关。
  function bindRecentList(list) {
    if (!list || list.getAttribute('data-bound')) return;
    list.setAttribute('data-bound', '1');
    list.addEventListener('click', function (e) {
      var btn = e.target.closest ? e.target.closest('[data-act]') : null;
      var row = e.target.closest ? e.target.closest('[data-key]') : null;
      if (!btn || !row) return;
      var key = row.getAttribute('data-key');
      // 按 键 而不是按 下标 取：轮询可能刚好在"渲染完 → 点下去"之间插进来改了数组
      var item = findRecentByKey(key);
      if (!item) { showToast('这条信号已经不在列表里了'); return; }
      var str = sequenceToStr(item);
      // "要发给设备/要写进规则"的路径必须用严格版：拿不到完整字符串就别动手，
      // 否则等于把这条规则的 B 悄悄换成残缺版（详见 sequenceToStrStrict 的注释）
      function strictStr() {
        var s = sequenceToStrStrict(item);
        if (!s) showToast('这条信号含时序码，但设备没给出原始时序数据，无法转成文本');
        return s;
      }

      switch (btn.getAttribute('data-act')) {
        case 'expand':
          var on = !row.classList.contains('expanded');
          row.classList.toggle('expanded', on);
          state.recentExpanded[key] = on;
          btn.textContent = on ? '收起' : '展开';
          return;
        case 'send':
          if (!needIp()) return;
          apiPost('/signals/send', { signals: item.signals, intervals: item.intervals || [] }, 'application/json')
            .then(function () { showToast('已发送'); }).catch(function () {});
          return;
        case 'set-a': {
          // 单条信号的 A 要的是"裸信号串"（不带 sequenceToStr 会加的首段间隔 @120;），
          // 多条信号的才用整串 —— 这个区别一直存在，不能顺手抹平
          if (item.signals && item.signals.length === 1) {
            if (!signalSerializable(item.signals[0])) {
              showToast('这条信号含时序码，但设备没给出原始时序数据，无法转成文本');
              return;
            }
            openNewRuleWith({ a: oneSignalToStr(item.signals[0]), b: '' });
            return;
          }
          var s = strictStr();
          if (!s) return;
          openNewRuleWith({ a: s, b: '' });
          return;
        }
        case 'set-b': {
          var s2 = strictStr();
          if (!s2) return;
          openNewRuleWith({ a: '', b: s2 });
          return;
        }
        case 'copy':
          copyToClipboard(str);
          return;
        case 'save': {
          var s3 = strictStr();
          if (!s3) return;
          state.pendingSaveSignal = s3;
          openSaveToDeviceModal();
          return;
        }
      }
    });
  }

  // 统一的"带着某个信号去新建规则"入口：A/B 谁填由调用方决定
  function openNewRuleWith(prefill) {
    if (state.rules.length >= MAX_RULES) {
      showToast('规则已达上限 ' + MAX_RULES + ' 条，请先删除一条');
      switchTab('rules');
      return;
    }
    openNewRuleForm(prefill && prefill.a ? prefill.a : '', prefill && prefill.b ? prefill.b : '');
    switchTab('rules');
  }

  // ── 规则表单草稿 ──────────────────────────────────────────────────────
  // 表单里写着的字是"还没提交的工作"，切页签不该把它冲掉。
  // 以前 showRulesView() 每次都用 state.pendingAddRuleA/B（多数时候是空串）
  // 覆盖输入框，而 switchTab('rules') 一定会走 loadRules() → showRulesView()。
  // 于是"新建规则 → 抓 A → 切到最近信号点『设为 B』→ 回来"这条**官方流程**
  // 必丢 A：抓 A 只写进了 input.value，从来没进过 pendingAddRuleA。
  // 现在输入框的值实时回写进草稿，渲染一律从草稿恢复。
  var ruleDraft = { id: null, name: '', a: '', b: '' };   // 编辑中
  var newRuleDraft = { name: '', a: '', b: '' };          // 新建中

  function bindDraft(inputId, draft, key) {
    var el = document.getElementById(inputId);
    if (!el) return;
    el.addEventListener('input', function () { draft[key] = el.value; });
  }

  function bindRuleDrafts() {
    bindDraft('rule-name', ruleDraft, 'name');
    bindDraft('rule-a', ruleDraft, 'a');
    bindDraft('rule-b', ruleDraft, 'b');
    bindDraft('new-rule-name', newRuleDraft, 'name');
    bindDraft('new-rule-a', newRuleDraft, 'a');
    bindDraft('new-rule-b', newRuleDraft, 'b');
  }

  function openNewRuleForm(a, b) {
    newRuleDraft.name = '';
    newRuleDraft.a = a || '';
    newRuleDraft.b = b || '';
    state.ruleNewVisible = true;
    state.ruleDetailId = null;
    showRulesView();
  }

  function closeNewRuleForm() {
    state.ruleNewVisible = false;
    newRuleDraft.name = '';
    newRuleDraft.a = '';
    newRuleDraft.b = '';
    stopCapture();
    showRulesView();
  }

  // ==================== 按遥控器抓码 ====================
  // 15 秒内收到第一条新信号就自动填进输入框。
  // 复用 /api/signals/recent 的 since 增量拉取，固件侧零改动。
  function stopCapture() {
    var ctx = state.captureAbort;
    if (!ctx) return;
    if (ctx.timer) clearInterval(ctx.timer);
    if (ctx.btn) {
      ctx.btn.disabled = false;
      ctx.btn.textContent = ctx.text;
    }
    state.captureAbort = null;
  }

  function startCapture(inputId, btnId, seconds) {
    if (!needIp()) return;
    stopCapture();
    var input = document.getElementById(inputId);
    var btn = document.getElementById(btnId);
    if (!input || !btn) return;

    var deadline = Date.now() + (seconds || 15) * 1000;
    var baseline = -1;
    var ctx = { timer: null, btn: btn, text: btn.textContent };
    state.captureAbort = ctx;
    btn.disabled = true;

    function finish(msg) {
      stopCapture();
      if (msg) showToast(msg);
    }

    function tick() {
      var left = deadline - Date.now();
      if (left <= 0) { finish('抓码超时：' + (seconds || 15) + ' 秒内没收到信号'); return; }
      btn.textContent = '请按遥控器… ' + Math.ceil(left / 1000) + 's';
      var url = '/signals/recent';
      if (baseline >= 0) url += '?since=' + baseline;
      apiGet(url).then(function (data) {
        var arr = data && data.data ? data.data : (Array.isArray(data) ? data : []);
        if (baseline < 0) {
          // 先取基线，避免把"抓码之前就存在"的旧信号误当成刚按下的
          var maxTs = 0;
          arr.forEach(function (it) { if (it.timestamp > maxTs) maxTs = it.timestamp; });
          baseline = maxTs;
          return;
        }
        if (!arr.length) return;
        var str = sequenceToStr(arr[arr.length - 1]);
        if (!str) return;
        input.value = str;
        // 抓码是"程序填值"，不会触发 input 事件 ⇒ 主动补一个，
        // 好让草稿同步（见 bindRuleDrafts），否则切个页签就把抓到的码丢了
        if (typeof Event === 'function') {
          try { input.dispatchEvent(new Event('input')); } catch (e) {}
        }
        finish('已抓到：' + signalSummary(str));
      }).catch(function () {});
    }

    tick();
    ctx.timer = setInterval(tick, 900);
  }

  // ==================== 转发记录 ====================

  function loadForwardRecords(quiet) {
    if (!needIp()) return;
    apiGet('/forward/records', { quiet: !!quiet }).then(function (data) {
      state.forwardRecords = data && data.data ? data.data : (Array.isArray(data) ? data : []);
      renderForwardRecords();
    }).catch(function () {
      state.forwardRecords = [];
      renderForwardRecords();
    });
    // 记录里只有规则 ID，规则名要另取一次（首次进「最近」页时 state.rules 还是空的）
    if (!state.rules.length) {
      apiGet('/maps', { quiet: true }).then(function (data) {
        state.rules = data && data.data ? data.data : (Array.isArray(data) ? data : []);
        renderForwardRecords();
      }).catch(function () {});
    }
  }

  function ruleNameById(id) {
    if (id == null || id < 0) return '';
    var r = state.rules.find(function (x) { return x.id === id; });
    return r && r.name ? r.name : '';
  }

  function aOfRecord(rec) {
    if (rec.aProtocol === 255) return '时序码';
    var proto = PROTOCOL_NAMES[rec.aProtocol] || ('协议' + rec.aProtocol);
    return proto + ' 0x' + (rec.aCode >>> 0).toString(16).toUpperCase();
  }

  function formatAge(sec) {
    if (sec == null) return '';
    if (sec < 5) return '刚刚';
    if (sec < 60) return sec + ' 秒前';
    if (sec < 3600) return Math.floor(sec / 60) + ' 分钟前';
    if (sec < 86400) return Math.floor(sec / 3600) + ' 小时前';
    return Math.floor(sec / 86400) + ' 天前';
  }

  function renderForwardRecords() {
    var list = document.getElementById('forward-list');
    var empty = document.getElementById('forward-empty');
    list.innerHTML = '';
    if (!state.forwardRecords.length) {
      empty.classList.remove('hidden');
      return;
    }
    empty.classList.add('hidden');
    state.forwardRecords.forEach(function (rec) {
      var info = FORWARD_RESULT_TEXT[rec.result] || { text: '未知', cls: '' };
      var name = ruleNameById(rec.ruleId);
      var title;
      if (rec.result === 0) {
        title = '命中规则 ' + (name ? '「' + name + '」' : ('#' + rec.ruleId)) + ' → 发出 ' + rec.bCount + ' 个信号';
      } else {
        title = info.text;
      }
      var meta = formatAge(rec.ageSec) + (rec.durationMs ? ' · 发射 ' + rec.durationMs + 'ms' : '');
      var div = document.createElement('div');
      div.className = 'list-item forward-item';
      div.innerHTML =
        '<span class="fwd-status ' + info.cls + '">' + escapeHtml(info.text) + '</span>' +
        '<span class="summary">' + escapeHtml(title) + '</span>' +
        '<span class="meta">A: ' + escapeHtml(aOfRecord(rec)) + (meta ? ' · ' + escapeHtml(meta) : '') + '</span>';
      list.appendChild(div);
    });
  }

  function clearForwardRecords() {
    if (!needIp()) return;
    apiRaw('DELETE', '/forward/records')
      .then(function (r) {
        if (!r.res.ok) throw new Error('failed');
        showToast('已清空');
        loadForwardRecords();
      })
      .catch(function () { showToast('清空失败'); });
  }

  // 清空最近信号（v1.3.24）。设备端是这个页面唯一的真数据源 ——
  // state.recent 只是缓存，所以设备端清空后**必须把本地那份也丢掉**，
  // 否则浏览器还留着旧条目，看着像"没清掉"。
  //
  // ⚠️ 还要重置 state.recentSig：renderRecent() 拿它当"内容没变就别重建 DOM"的指纹
  // （实时监视 1.2s 拉一次，不重建是为了不闪）。清空后列表变空但指纹还是旧的，
  // 列表就不会被清掉 —— 这个坑和"展开状态丢失"是同一处代码的两个用途。
  //
  // ⚠️ 设备端 DELETE 之后没有 undo：清空期间按的遥控器会重新出现在列表里。
  function clearRecentSignals() {
    if (!needIp()) return;
    var btn = document.getElementById('btn-clear-recent');
    if (btn) btn.disabled = true;
    apiRaw('DELETE', '/signals/recent')
      .then(function (r) {
        if (!r.res.ok) throw new Error('failed');
        state.recent = [];
        state.recentSig = null;
        state.recentExpanded = {};
        showToast('已清空');
        renderRecent(true);
      })
      .catch(function () { showToast('清空失败'); })
      .then(function () { if (btn) btn.disabled = false; });
  }

  function forwardTest() {
    var a = document.getElementById('test-a').value.trim();
    if (!a) { showToast('请输入 A 信号'); return; }
    if (!needIp()) return;
    var box = document.getElementById('forward-test-result');
    box.classList.remove('hidden');
    box.className = 'status-box';
    box.textContent = '正在试发…';
    apiPost('/forward/test', { a: a }, 'application/json').then(function (data) {
      box.className = 'status-box test-ok';
      box.textContent = (data && data.data ? data.data : '已完成') + '（已记进下面的转发记录）';
      loadForwardRecords();
    }).catch(function (err) {
      box.className = 'status-box test-err';
      box.textContent = (err && err.message && err.message !== 'request failed') ? err.message : '试发失败';
      loadForwardRecords();
    });
  }

  function renderSaveDeviceOptions() {
    var sel = document.getElementById('save-device-select');
    var prev = sel.value;
    sel.innerHTML = '<option value="">-- 选择设备 --</option>';
    state.devices.forEach(function (d) {
      var opt = document.createElement('option');
      opt.value = d.id;
      opt.textContent = d.name;
      sel.appendChild(opt);
    });
    // 数据回来后重建时，尽量保住用户已经选中的那一项
    if (prev && state.devices.some(function (d) { return String(d.id) === prev; })) sel.value = prev;
  }

  // defaults: { deviceName, buttonName } —— 「保存到设备」把刚学到的名字带进来，
  // 免得用户再手打一遍（手打极易和已有设备名差一个字，变成两个设备）。
  function openSaveToDeviceModal(defaults) {
    defaults = defaults || {};
    var wantName = (defaults.deviceName || '').trim();
    document.getElementById('save-button-name').value = defaults.buttonName || '';
    // 设备名先按名字填进"新建设备"框；等设备端列表真的回来，若已有同名设备，
    // 再改成"选中那个设备"并清空输入框 —— 否则会凭空多出一个同名设备。
    document.getElementById('save-device-new-name').value = wantName;

    // ⚠️ 以前是 loadDevices() 之后**立刻**用 state.devices 建下拉框。而 loadDevices()
    // 里那次"以设备为准"的同步是异步的（要先等 /api/devices 回来），所以下拉框里
    // 永远是**上一次**那份列表：刚新建的设备选不到，刚删掉的设备还留在里面。
    // 现在先用本地缓存渲染（立刻可用），设备端数据回来后再重建一次。
    document.getElementById('modal-overlay').classList.remove('hidden');
    document.getElementById('modal-save-device').classList.remove('hidden');
    renderSaveDeviceOptions();
    loadDevicesThen(function () {
      renderSaveDeviceOptions();
      if (!wantName) return;
      var existing = state.devices.filter(function (d) { return d.name === wantName; })[0];
      if (existing) {
        document.getElementById('save-device-select').value = String(existing.id);
        document.getElementById('save-device-new-name').value = '';
      }
    });
  }

  // 刚学习的设备名：优先用「学习」表单里填的，其次用上次记下的（页面刷新后表单是空的）
  function lastLearnDeviceName() {
    var v = (document.getElementById('learn-device-name').value || '').trim();
    if (v) return v;
    try { return localStorage.getItem(STORAGE_LEARN_DEVICE) || ''; } catch (e) { return ''; }
  }

  function closeSaveToDeviceModal() {
    document.getElementById('modal-overlay').classList.add('hidden');
    document.getElementById('modal-save-device').classList.add('hidden');
    state.pendingSaveSignal = null;
  }

  function confirmSaveToDevice() {
    var signal = state.pendingSaveSignal;
    if (!signal) { closeSaveToDeviceModal(); return; }
    var sel = document.getElementById('save-device-select');
    var newName = document.getElementById('save-device-new-name').value.trim();
    var btnName = document.getElementById('save-button-name').value.trim();
    if (!btnName) { showToast('请输入按钮名'); return; }
    var devices = state.devices.slice();
    var targetDevice = null;
    var selId = sel.value ? parseInt(sel.value, 10) : null;

    // ⭐⭐ 判据是「设备里有没有**同名**设备」，不是「"新建设备"输入框空不空」（v1.3.46 修）。
    //   旧实现：`if (newName) 就新建`。而弹窗打开时就把学习时填的设备名**预填**进那个框，
    //   要等 /api/devices 回来才清空 ⇒ 在它回来之前（或那次同步失败时，比如设备正忙/正在重启）
    //   点保存，就**凭空多出一个同名设备**。用户实测："一次学习之后设备页出现了三个设备"
    //   （电源+风速 / 摇头 / 定时，全是「艾美特风扇」）。
    //   现在：明确选了下拉框就尊重它；没选却填了名字 ⇒ 先按名字找，找到就并进去，找不到才新建。
    if (selId) {
      targetDevice = devices.find(function (d) { return d.id === selId; }) || null;
      if (!targetDevice) { showToast('请选择设备'); return; }
    } else if (newName) {
      targetDevice = devices.filter(function (d) { return d.name === newName; })[0] || null;
      if (!targetDevice) {
        var newId = devices.length ? Math.max.apply(null, devices.map(function (d) { return d.id; })) + 1 : 1;
        targetDevice = { id: newId, name: newName, buttons: [] };
        devices.push(targetDevice);
      }
    } else {
      showToast('请选择设备或输入新设备名');
      return;
    }

    targetDevice.buttons = targetDevice.buttons || [];
    // 同名按钮**换信号**（"重新学了一遍"的语义），不同名才追加 —— 免得冒出两个同名按钮。
    // 与设备页那条「一键加入学到的指令」保持同一套规则。
    var exist = targetDevice.buttons.filter(function (b) { return b.name === btnName; })[0];
    if (exist) exist.signal = signal;
    else targetDevice.buttons.push({ name: btnName, signal: signal });
    state.devices = devices;
    // 之前这里无条件弹"已保存到设备"，而写设备失败被 .catch 静默吞掉 —— 名不副实。
    persistDevices(devices).then(function (ok) {
      showToast(ok ? '已保存到设备' : '已存到本地，但没写进设备（换手机会丢）');
    });
    closeSaveToDeviceModal();
    if (state.tab === 'devices') renderDevicesList();
  }

  function learnStart() {
    if (!needIp()) return;
    var deviceName = document.getElementById('learn-device-name').value.trim();
    var commandNames = document.getElementById('learn-command-names').value.trim();
    var timeout = parseInt(document.getElementById('learn-timeout').value, 10) || 30;
    if (!commandNames) { showToast('请输入指令名'); return; }
    var effectiveName = deviceName || 'Learned';
    // 记下设备名，供「保存到设备」预填（页面刷新后表单会清空，靠这份兜底）
    try { localStorage.setItem(STORAGE_LEARN_DEVICE, effectiveName); } catch (e) {}
    apiPost('/learn/start', {
      deviceName: effectiveName,
      commandNames: commandNames,
      timeoutSeconds: timeout
    }, 'application/json').then(function () {
      showToast('学习已启动');
      document.getElementById('learn-form-box').classList.add('hidden');
      document.getElementById('btn-learn-start').classList.add('hidden');
      document.getElementById('btn-learn-stop').classList.remove('hidden');
      document.getElementById('learn-status').classList.remove('hidden');
      startLearnPoll();
    }).catch(function () {});
  }

  // 把学习界面恢复成"可以重新开始"的样子（三处收尾共用）
  function restoreLearnUi() {
    document.getElementById('learn-form-box').classList.remove('hidden');
    document.getElementById('btn-learn-start').classList.remove('hidden');
    document.getElementById('btn-learn-stop').classList.add('hidden');
  }

  var LEARN_POLL_MAX_FAILS = 5;

  function startLearnPoll() {
    var fails = 0;
    startPoller('learn', function () {
      if (!getBaseUrl()) return Promise.resolve();
      return apiGet('/learn/status', { quiet: true }).then(function (data) {
        fails = 0;
        var d = data && data.data ? data.data : data;
        state.learnStatus = d;
        var statusEl = document.getElementById('learn-status');
        statusEl.innerHTML = '学习中: ' + (d.currentCommand || '') + ' — ' + (d.progress || '');
        if (!d.isLearning) {
          stopLearnPoll();
          restoreLearnUi();
          loadLearnResult();
        }
      }).catch(function () {
        // ⚠️ 以前这里只是 catch(function(){})：设备中途自己重启（内存见底时的
        // HealthGuard 自愈）之后，这个 1.5s 的轮询会**永不停歇**地打一个
        // 早就不在学习状态的设备，每轮还弹一条报错 toast，直到用户关页面。
        // 连续失败到上限就收工，把界面交还给用户。
        fails++;
        if (fails >= LEARN_POLL_MAX_FAILS) {
          stopLearnPoll();
          restoreLearnUi();
          document.getElementById('learn-status').classList.add('hidden');
          // ⭐ 收工前把结果读一次：设备重启过的话，学习结果现在**不会丢**了
          //   （v1.3.46 起学完就落盘到设备闪存），所以这里很可能读得到，
          //   而旧实现直接收工、什么都不读 ⇒ 界面上看着像白学。
          loadLearnResult();
          showToast('拿不到学习状态（设备可能刚重启过），可以重新开始学习');
        }
      });
    }, 1500);
  }

  function stopLearnPoll() {
    stopPoller('learn');
  }

  function learnStop() {
    if (!needIp()) return;
    apiPost('/learn/stop', {}, 'application/json').then(function () {
      showToast('已停止学习');
      stopLearnPoll();
      restoreLearnUi();
      document.getElementById('learn-status').classList.add('hidden');
      // ⚠️ 手动点「停止学习」也必须把结果取回来。以前只有"超时自动结束"那条路会
      //    加载结果（见 startLearnPoll），手动停的话界面上什么都不会出现，
      //    看着像"白学了一场"。
      loadLearnResult();
    }).catch(function () {});
  }

  // 读取学习结果。
  //
  // ⚠️⚠️ 设备返回的 `data` 是**字符串**（SmartIR JSON 文本），不是对象 ——
  //   以前写 `obj.commands`，而 obj 是字符串 ⇒ 恒 undefined ⇒ 结果列表永远是空的
  //   ⇒ 用户看到的正是"学习完成了但不显示学习结果"。必须 JSON.parse。
  //   （固件侧 v1.3.28 已把这条接口统一成**恒定**返回字符串：更早的版本内存够时返对象、
  //    不够时返字符串，同一个取法时而灵时不灵，那才是最坑的形态。）
  //
  // ⚠️ 另一件事：设备端取结果要过一次内存检查，而"刚学完"正是内存最低的时刻
  //   （学习有一笔一次性台阶）⇒ 这一下可能直接返回 503。碎片会随空闲逐渐合并，
  //   所以失败后自动重试几次，而不是立刻告诉用户"读不到"。
  // ⭐ 重试预算按"设备自行重启一次"来定：实测学完 3~4 分钟设备会因内存不足重启，
  //   重启到能应答约 15 秒 ⇒ 4 次 × 1.5 s（6 s）根本等不到，改成 8 次 × 2 s（约 16 s）。
  var LEARN_RESULT_MAX_ATTEMPTS = 8;
  var LEARN_RESULT_RETRY_MS = 2000;

  // silent=true：切页签时的自动加载。没学过会 404，不该弹提示骚扰用户。
  function loadLearnResult(attempt, silent) {
    if (!needIp()) return Promise.resolve();
    var n = attempt || 0;
    return apiGet('/learn/result?format=string', { quiet: true }).then(function (data) {
      var obj = (data && data.data !== undefined) ? data.data : data;
      if (typeof obj === 'string') {
        try { obj = JSON.parse(obj); } catch (e) { obj = null; }
      }
      state.learnResult = (obj && obj.commands) ? obj.commands : {};
      renderLearnResult();
      applyLearnResultVisibility();
      renderDevicesLearnHint();     // 设备页那条「还没进设备」的提示条也靠这份结果
      // 学到东西就催一句。⚠️ 别再说"重启会丢"——v1.3.46 起结果已落盘，
      // 那句话现在是错的（会让人以为必须赶时间）。
      var got = Object.keys(state.learnResult).length;
      if (!silent && got) {
        showToast('学到 ' + got + ' 条：请「设为 B」建规则或「保存到设备」');
      }
    }).catch(function (err) {
      var msg = (err && err.message) || '';
      var status = err && err.status;
      var ename = (err && err.name) || '';
      // ⭐ 可重试的两类：① 设备**当时**内存不够（503 / "Insufficient memory"）；
      //   ② 设备没应答 —— 正在重启、或内存见底到 accept 了却不回话（AbortError / Failed to fetch）。
      //   ⚠️⚠️ 旧实现只认第 ① 类：设备重启中时错误信息里没有 "memory" ⇒ 直接清空结果、
      //   **一句提示都不给**。而"学完 3~4 分钟设备会因内存不足自行重启"是已知的高频路径
      //   （见固件 `ir_learner.cpp` 的 persistLearnedResult 注释）⇒ 用户看到的就是
      //   "学习完成了，什么都不显示，也不知道为什么"。现在两类都重试、也都说清楚。
      var transient = /memory/i.test(msg) || status === 503 || ename === 'AbortError' ||
                      /Failed to fetch|NetworkError|Load failed|aborted/i.test(msg);
      if (transient && n + 1 < LEARN_RESULT_MAX_ATTEMPTS) {
        setTimeout(function () { loadLearnResult(n + 1, silent); }, LEARN_RESULT_RETRY_MS);
        return;
      }
      state.learnResult = {};
      renderLearnResult();
      applyLearnResultVisibility();
      renderDevicesLearnHint();
      if (!silent && transient) {
        showToast('暂时读不到学习结果：设备正在重启或内存紧张。等约半分钟再点「刷新结果」');
      }
    });
  }

  // 清除学习结果（v1.3.50）——落盘的结果会一直留在设备闪存里（约 1 KB、不占内存），
  // 开始下一次学习才会自动覆盖。给一个"我就是不想要了"的入口。
  // ⚠️ 只清"暂存的学习结果"，**不动**已经「保存到设备」的按钮、也不动已建的规则。
  function clearLearnResult() {
    if (!needIp()) return;
    if (!confirm('清除设备里保存的学习结果？\n\n已经「保存到设备」的按钮、以及已经建好的规则都不受影响。')) return;
    apiRaw('DELETE', '/learn/result')
      .then(function (r) {
        if (!r.res.ok) {
          throw new Error((r.data && r.data.error) || 'failed');
        }
        state.learnResult = {};
        renderLearnResult();
        applyLearnResultVisibility();
        renderDevicesLearnHint();
        showToast('学习结果已清除');
      })
      .catch(function (err) {
        showToast('清除失败' + (err && err.message ? '：' + err.message : ''));
      });
  }

  // 有内容才展开结果区。以前是无条件展开 ⇒ 没学过也会冒出一个空壳。
  function applyLearnResultVisibility() {
    var box = document.getElementById('learn-result-box');
    if (!box) return;
    if (Object.keys(state.learnResult || {}).length) box.classList.remove('hidden');
    else box.classList.add('hidden');
  }

  function renderLearnResult() {
    var list = document.getElementById('learn-result-list');
    list.innerHTML = '';
    var commands = state.learnResult || {};
    Object.keys(commands).forEach(function (cmdName) {
      var str = commands[cmdName];
      if (typeof str !== 'string') return;
      var div = document.createElement('div');
      div.className = 'list-item';
      div.innerHTML =
        '<span class="summary">' + escapeHtml(cmdName) + '</span>' +
        '<span class="signal-str">' + escapeHtml(str) + '</span>' +
        '<div class="actions">' +
        '<button type="button" class="btn btn-primary btn-send-learn">发送</button>' +
        '<button type="button" class="btn btn-secondary btn-set-a-learn">设为 A</button>' +
        '<button type="button" class="btn btn-secondary btn-set-b-learn">设为 B</button>' +
        '<button type="button" class="btn btn-secondary btn-copy-learn">复制</button>' +
        '<button type="button" class="btn btn-secondary btn-save-learn">保存到设备</button>' +
        '</div>';
      div.querySelector('.btn-send-learn').addEventListener('click', function () {
        if (!needIp()) return;
        apiPost('/signals/send', str, 'text/plain').then(function () { showToast('已发送'); }).catch(function () {});
      });
      div.querySelector('.btn-copy-learn').addEventListener('click', function () {
        copyToClipboard(str);
      });
      div.querySelector('.btn-save-learn').addEventListener('click', function () {
        state.pendingSaveSignal = str;
        openSaveToDeviceModal({ deviceName: lastLearnDeviceName(), buttonName: cmdName });
      });
      div.querySelector('.btn-set-a-learn').addEventListener('click', function () {
        openNewRuleWith({ a: str, b: '' });
      });
      div.querySelector('.btn-set-b-learn').addEventListener('click', function () {
        openNewRuleWith({ a: '', b: str });
      });
      list.appendChild(div);
    });
  }

  function loadRules(quiet) {
    if (!needIp()) return;
    apiGet('/maps', { quiet: !!quiet }).then(function (data) {
      var arr = data && data.data ? data.data : (Array.isArray(data) ? data : []);
      state.rules = arr;
      showRulesView();
    }).catch(function () {
      state.rules = [];
      showRulesView();
    });
  }

  // 规则数上限：必须与固件的 MAX_MAP_RULES 保持一致，否则提示会骗人
  var MAX_RULES = 20;

  function renderRulesLimitHint() {
    var host = document.getElementById('rules-limit-hint');
    if (!host) return;
    var n = state.rules.length;
    if (n < MAX_RULES - 2) {          // 还剩 3 条以上就不打扰
      host.classList.add('hidden');
      return;
    }
    host.classList.remove('hidden');
    if (n >= MAX_RULES) {
      host.className = 'hint warn';
      host.textContent = '规则已达上限 ' + MAX_RULES + ' 条，需先删除一条才能新建';
    } else {
      host.className = 'hint';
      host.textContent = '规则 ' + n + '/' + MAX_RULES + ' 条，接近上限';
    }
  }

  function showRulesView() {
    var listWrap = document.getElementById('rules-list');
    var empty = document.getElementById('rules-empty');
    var detail = document.getElementById('rule-detail');
    var newForm = document.getElementById('rule-new-form');

    detail.classList.add('hidden');
    newForm.classList.add('hidden');
    listWrap.classList.remove('hidden');
    empty.classList.remove('hidden');

    if (state.ruleDetailId != null) {
      var rule = state.rules.find(function (r) { return r.id === state.ruleDetailId; });
      if (rule) {
        listWrap.classList.add('hidden');
        empty.classList.add('hidden');
        detail.classList.remove('hidden');
        // 换了一条规则才从服务端取；同一条规则一律用草稿 ⇒ 改到一半切走再回来不丢
        if (ruleDraft.id !== state.ruleDetailId) {
          ruleDraft.id = state.ruleDetailId;
          ruleDraft.name = rule.name || '';
          ruleDraft.a = aSignalToStr(rule.aSignal);
          ruleDraft.b = bSequenceToStr(rule.bSequence);
        }
        document.getElementById('rule-name').value = ruleDraft.name;
        document.getElementById('rule-a').value = ruleDraft.a;
        document.getElementById('rule-b').value = ruleDraft.b;
        return;
      }
    }

    if (state.ruleNewVisible) {
      listWrap.classList.add('hidden');
      empty.classList.add('hidden');
      newForm.classList.remove('hidden');
      document.getElementById('new-rule-name').value = newRuleDraft.name;
      document.getElementById('new-rule-a').value = newRuleDraft.a;
      document.getElementById('new-rule-b').value = newRuleDraft.b;
      return;
    }

    renderRulesLimitHint();
    listWrap.innerHTML = '';
    if (!state.rules.length) {
      empty.classList.remove('hidden');
      return;
    }
    empty.classList.add('hidden');
    state.rules.forEach(function (r) {
      var aStr = aSignalToStr(r.aSignal);
      var bStr = bSequenceToStr(r.bSequence);
      var div = document.createElement('div');
      div.className = 'list-item rule-item';
      div.innerHTML =
        '<div><span class="name">' + escapeHtml(r.name || '规则 ' + r.id) + '</span><br><span class="ab">A: ' + escapeHtml(signalSummary(aStr)) + ' → B: ' + escapeHtml(signalSummary(bStr)) + '</span></div>' +
        '<button type="button" class="btn btn-secondary">编辑</button>';
      div.querySelector('.btn').addEventListener('click', function () {
        state.ruleDetailId = r.id;
        state.ruleNewVisible = false;
        showRulesView();
      });
      listWrap.appendChild(div);
    });
  }

  function saveRule() {
    var id = state.ruleDetailId;
    if (id == null) return;
    var name = document.getElementById('rule-name').value.trim();
    var a = document.getElementById('rule-a').value.trim();
    var b = document.getElementById('rule-b').value.trim();
    if (!a || !b) { showToast('请填写 A 和 B'); return; }
    // 编辑表单里的 A/B 是拿设备数据**反向序列化**出来的。若原规则里有时序信号
    // 而设备没给出 timingBase64，反序列化必然是残缺的 —— 这时点保存等于亲手把
    // 它改坏。宁可拦住让用户重新抓一次，也不要静默丢数据。
    var orig = state.rules.find(function (r) { return r.id === id; });
    if (orig && (!signalSerializable(orig.aSignal) || !sequenceSerializable(orig.bSequence))) {
      showToast('这条规则的原始码含时序数据且设备未给出全量，直接保存会改坏它，请重新抓一次 A/B');
      return;
    }
    apiPut('/maps', { id: id, rule: { name: name || undefined, a: a, b: b } }).then(function () {
      showToast('已保存');
      state.ruleDetailId = null;
      ruleDraft.id = null;
      loadRules();
    }).catch(function () {});
  }

  function deleteRule() {
    var id = state.ruleDetailId;
    if (id == null) return;
    if (!confirm('确定删除该规则？')) return;
    // ⚠️ 以前这里是"把剩下的规则全量 PUT 回去"。全量语义是"先删光再写"，
    // 只要有一条序列化不出来（见 sequenceToStrStrict），那一条就被静默丢掉了。
    // 固件 v1.3.16 已经支持 mode:"delete"，删一条就只删这一条。
    apiPut('/maps', { mode: 'delete', id: id }).then(function () {
      showToast('已删除');
      state.ruleDetailId = null;
      ruleDraft.id = null;
      loadRules();
    }).catch(function () {});
  }

  function saveNewRule() {
    var name = document.getElementById('new-rule-name').value.trim();
    var a = document.getElementById('new-rule-a').value.trim();
    var b = document.getElementById('new-rule-b').value.trim();
    if (!a || !b) { showToast('请填写 A 和 B'); return; }
    // ⚠️ 以前这里也是全量 PUT：把现有 20 条规则重新序列化一遍再连同新规则发过去。
    // 那条路径上有两个隐患：① 序列化失败的规则会被静默换掉；② 设备正在并发处理
    // 别的请求时，一次 20 条的全量写要重写整个文件，很容易写到一半失败（旧规则也没了）。
    // 固件 v1.3.16 的 mode:"append" 只追加一条，新 id 由设备分配。
    apiPut('/maps', { mode: 'append', rules: [{ name: name || undefined, a: a, b: b }] }).then(function () {
      showToast('已添加规则');
      state.ruleNewVisible = false;
      newRuleDraft.name = '';
      newRuleDraft.a = '';
      newRuleDraft.b = '';
      loadRules();
    }).catch(function () {});
  }

  // ── 备份：导出 / 导入（设备与规则）─────────────────────────────────────
  // 为什么放在前端做：设备/规则的全量数据本来就会先落到浏览器（一次 GET 就有），
  // 生成文件、下载、读文件都不需要固件配合，所以不加接口、不动固件协议。
  //
  // ⚠️ 导入一律是**合并**，不是"先清空再导入"。后者等于用户点一下就把现有配置删了，
  //    文件选错没有后悔药；合并是可重复、可回退的（同样的文件导两次不会翻倍）。
  function backupStamp() {
    var d = new Date();
    function p(n) { return (n < 10 ? '0' : '') + n; }
    return d.getFullYear() + p(d.getMonth() + 1) + p(d.getDate()) + '-' + p(d.getHours()) + p(d.getMinutes());
  }

  function downloadJson(filename, obj) {
    var blob = new Blob([JSON.stringify(obj, null, 2)], { type: 'application/json' });
    var url = URL.createObjectURL(blob);
    var a = document.createElement('a');
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    setTimeout(function () { URL.revokeObjectURL(url); }, 1000);
  }

  function pickJsonFile(inputId, onObj) {
    var input = document.getElementById(inputId);
    if (!input) return;
    input.onchange = function () {
      var f = input.files && input.files[0];
      input.value = '';        // 先复位，允许连续选同一个文件
      if (!f) return;
      if (f.size > 512 * 1024) { showToast('文件太大（超过 512 KB）'); return; }
      var reader = new FileReader();
      reader.onload = function () {
        var obj;
        try { obj = JSON.parse(reader.result); }
        catch (e) { showToast('这个文件不是合法 JSON'); return; }
        onObj(obj);
      };
      reader.onerror = function () { showToast('读文件失败'); };
      reader.readAsText(f);
    };
    input.click();
  }

  function exportDevices() {
    if (!state.devices.length) { showToast('还没有设备可导出'); return; }
    downloadJson('ct30w-devices-' + backupStamp() + '.json', {
      kind: 'ct30w-devices',
      version: 1,
      exportedAt: new Date().toISOString(),
      devices: state.devices
    });
    showToast('已导出 ' + state.devices.length + ' 个设备');
  }

  function importDevicesFrom(obj) {
    if (!obj || obj.kind !== 'ct30w-devices' || !Array.isArray(obj.devices)) {
      showToast('这不是「设备」的备份文件'); return;
    }
    var merged = state.devices.map(function (d) {
      return { id: d.id, name: d.name, buttons: (d.buttons || []).slice() };
    });
    var nextId = merged.length ? Math.max.apply(null, merged.map(function (d) { return d.id; })) + 1 : 1;
    var newDev = 0, newBtn = 0;
    obj.devices.forEach(function (src) {
      if (!src || typeof src.name !== 'string' || !src.name) return;
      var dst = merged.filter(function (d) { return d.name === src.name; })[0];
      if (!dst) { dst = { id: nextId++, name: src.name, buttons: [] }; merged.push(dst); newDev++; }
      (src.buttons || []).forEach(function (b) {
        if (!b || typeof b.name !== 'string' || typeof b.signal !== 'string') return;
        if (dst.buttons.some(function (x) { return x.name === b.name; })) return;   // 同名按钮不重复加
        dst.buttons.push({ name: b.name, signal: b.signal });
        newBtn++;
      });
    });
    if (!newDev && !newBtn) { showToast('文件里的设备这里都已经有了，没有新增'); return; }
    if (!confirm('导入：新增 ' + newDev + ' 个设备、' + newBtn + ' 个按钮。\n同名设备/按钮会跳过，现有内容不会被删除。继续？')) return;
    state.devices = merged;
    persistDevices(merged).then(function (ok) {
      showToast(ok ? '已导入并写进设备' : '已导入到本机，但没写进设备（换手机会丢）');
    });
    renderDevicesView();
  }

  function exportRules() {
    if (!state.rules.length) { showToast('还没有规则可导出'); return; }
    var out = [];
    var incomplete = 0;
    state.rules.forEach(function (r) {
      var a = aSignalToStr(r.aSignal);
      var b = bSequenceToStr(r.bSequence);
      // 设备没给出全量时序数据时，这条导出出去就是残缺的 —— 数出来，别让用户
      // 以为备份是完整的（同一个判据在保存规则时用来拦截，见 sequenceToStrStrict）。
      if (!a || !b || !signalSerializable(r.aSignal) || !sequenceSerializable(r.bSequence)) incomplete++;
      out.push({ name: r.name || '', a: a, b: b });
    });
    downloadJson('ct30w-rules-' + backupStamp() + '.json', {
      kind: 'ct30w-rules',
      version: 1,
      exportedAt: new Date().toISOString(),
      rules: out
    });
    showToast(incomplete
      ? ('已导出 ' + out.length + ' 条，其中 ' + incomplete + ' 条原始码不完整')
      : ('已导出 ' + out.length + ' 条规则'));
  }

  function importRulesFrom(obj) {
    if (!obj || obj.kind !== 'ct30w-rules' || !Array.isArray(obj.rules)) {
      showToast('这不是「规则」的备份文件'); return;
    }
    var incoming = obj.rules.filter(function (r) {
      return r && typeof r.a === 'string' && typeof r.b === 'string' && r.a && r.b;
    });
    if (!incoming.length) { showToast('文件里没有可用的规则'); return; }
    // 按**名称**判重：规则 id 是设备本地分配的，换台设备就对不上了，名字才是语义标识。
    var names = {};
    state.rules.forEach(function (r) { if (r.name) names[r.name] = true; });
    var fresh = incoming.filter(function (r) { return !r.name || !names[r.name]; });
    var skipped = incoming.length - fresh.length;
    if (!fresh.length) { showToast('文件里的规则这里都已经有了（按名称判断）'); return; }
    var room = MAX_RULES - state.rules.length;
    if (room <= 0) { showToast('规则已达上限 ' + MAX_RULES + ' 条，请先删掉一些再导入'); return; }
    var toAdd = fresh.slice(0, room);
    var overflow = fresh.length - toAdd.length;
    var msg = '导入 ' + toAdd.length + ' 条规则';
    if (skipped) msg += '（按名称跳过已有的 ' + skipped + ' 条）';
    if (overflow) msg += '；另有 ' + overflow + ' 条超出上限 ' + MAX_RULES + ' 未导入';
    if (!confirm(msg + '。继续？')) return;
    // 批量 append：固件在这一次请求里统一落盘一次，不会"每加一条重写整个文件"
    apiPut('/maps', {
      mode: 'append',
      rules: toAdd.map(function (r) { return { name: r.name || undefined, a: r.a, b: r.b }; })
    }).then(function () {
      showToast('已导入 ' + toAdd.length + ' 条规则');
      loadRules();
    }).catch(function () {});
  }

  // ── 「导出给 HA」：两种格式，由页面上的下拉框选 ─────────────────────────
  //
  // ⭐ 背景（曾被搞错，记在这里免得再错一遍）：
  //    SmartIR 的 `commandsEncoding` **不约束 commands 值的内容**。它只是个"声明"，
  //    甚至 `check_encoding()` 只校验这个字符串在控制器的允许列表里，**根本不看值**。
  //    真正决定值怎么用的是 **controller**：
  //      · MQTT 控制器 —— 把值**原样 publish** 到 controller_data 那个主题，不做任何转换；
  //      · Broadlink / 小米 / ESPHome —— 才会去解析"微秒波形"。
  //    所以走 MQTT 时，值可以直接放**我们自己的转发串**（HA 里那份海信电视配置就是这么写的：
  //    "volumeUp": "1,0xFD22DD"，正是本机语法）。Community 里更极端的例子是
  //    MQTT + Raw 的 payload 直接写成 {"Protocol":"SAMSUNG",...}，一样能用。
  //
  // ⭐ 本固件早就为此开了口子：订阅这四个通配主题，收到**纯字符串**（不过 JSON 包装）
  //    就直接 forwarder.parseAndSendSignal()。
  //        smartir/climate/+/send      smartir/media_player/+/send
  //        smartir/fan/+/send          smartir/light/+/send
  //    ⇒ HA 侧 configuration.yaml 里把 controller_data 填成
  //      smartir/media_player/<名字>/send （按平台换 climate / fan / light）即可端到端打通。
  //
  // 于是有了两条路：
  //   ① native —— commands 的值 = 设备里存的那条串，**一字不改**。
  //      ✅ 零转换 ⇒ 任何协议都能导（base64 时序码 / 1,0xHEX / @ms; 序列 / JSON），
  //         不存在"波形算错"这种错法；正确性可以用"字符串相等"来验收。
  //   ② raw    —— 展开成**微秒波形**（带符号的 JSON 数组，如 "[9000,-4500,560,…]"），
  //      给 ESPHome / LOOKin / 小米这类需要真实波形的控制器。
  //      ⚠️ 为什么必须是"带符号 + 方括号"：SmartIR 的 ESPHome 控制器对值做的是
  //         `json.loads(command)`（见 controller.py），裸的 "9000,4500,…" 会直接抛异常；
  //         它自家 codes/ 里的 Raw 码也正是这种写法（例：codes/fan/7040.json 的
  //         "off": "[1238, -438, 1238, -439, …]"）。带符号还顺便让"波形以熄灭开头"
  //         变得可表示（负号就是熄灭），不必再用"补一个 0 长度发光"那种补丁。
  //      要自己算波形 ⇒ 只做能保证正确的两类输入：**时序码**（base64 / 数组）与
  //      **NEC 协议码**；其余协议（RC5/RC6/Sony/Samsung）返回 null ⇒ 少给一条并报出来。
  //      绝不给一条波形是错的：错的波形比没有更坏，它会**静默打到被控设备上**。
  var SMARTIR_BURST_GAP_US = 7270;   // 必须与固件 TIMING_BURST_GAP_US 一致
  // 必须与固件 MQTT_MAX_INBOUND_PAYLOAD 一致：超过这条线的 payload 会被
  // handleSmartIRSignal() 直接扔掉（只在串口留一行），HA 那边看起来"发出去了但没反应"。
  // 导出一个超长的值 = 交付一个哑文件，所以这里就把它拦下来并报出来。
  var SMARTIR_MAX_PAYLOAD = 768;

  function timingBase64ToRawSamples(b64) {
    var bin;
    try { bin = atob(b64); } catch (e) { return null; }
    if (!bin.length || bin.length % 2 !== 0) return null;
    var out = [];
    for (var i = 0; i < bin.length; i += 2) {
      var v = (bin.charCodeAt(i) & 0xff) | ((bin.charCodeAt(i + 1) & 0xff) << 8);
      out.push(v * 10);            // 设备里存的是「微秒 / 10」（见 timingToBase64）
    }
    return out;
  }

  // NEC：9000/4500 引导 + 32 位（1 = 560/1690，0 = 560/560）+ 收尾 560
  function necToRawSamples(data) {
    var raw = [9000, 4500];
    for (var bit = 31; bit >= 0; bit--) {
      raw.push(560, ((data >>> bit) & 1) ? 1690 : 560);
    }
    raw.push(560);
    return raw;
  }

  // 单个信号段 -> [{level, usec}]；不认识就返回 null
  function signalSegmentToEvents(seg) {
    var s = (seg || '').trim();
    if (!s) return null;
    if (s.charAt(0) === '[') {                       // 时序数组：已是微秒
      var nums = s.replace(/^\[|\]$/g, '').split(',');
      var out = [];
      var level = true;
      for (var i = 0; i < nums.length; i++) {
        var v = parseFloat(nums[i]);
        if (!isFinite(v)) return null;
        // 数组格式的约定：正数=亮，负数=灭
        out.push({ level: v >= 0, usec: Math.abs(v) });
      }
      if (!out.length) return null;
      return out;
    }
    var proto = s.match(/^(\d+)\s*,\s*0x([0-9a-fA-F]+)\s*(?:,\s*(\d+))?$/);
    if (proto) {
      var p = parseInt(proto[1], 10);
      if (p !== 1) return null;                      // 只支持 NEC
      var data = parseInt(proto[2], 16) >>> 0;
      var samples = necToRawSamples(data);
      var ev = [];
      for (var k = 0; k < samples.length; k++) ev.push({ level: (k % 2 === 0), usec: samples[k] });
      return ev;
    }
    // 其余当作 Base64 时序码
    var rawSamples = timingBase64ToRawSamples(s);
    if (!rawSamples) return null;
    var e2 = [];
    for (var m = 0; m < rawSamples.length; m++) e2.push({ level: (m % 2 === 0), usec: rawSamples[m] });
    return e2;
  }

  // 整串 -> { events, seqRepeat }；失败返回 null
  function signalStringToEvents(str) {
    var s = (str || '').trim();
    if (!s) return null;
    var seqRepeat = 1;
    var pipe = s.lastIndexOf('|');
    if (pipe > 0) {
      var n = parseInt(s.slice(pipe + 1), 10);
      if (isFinite(n) && n > 0) seqRepeat = n;
      s = s.slice(0, pipe).trim();
    }
    var events = [];
    if (s.charAt(0) === '@') {                       // 前导 @毫秒;
      var semi = s.indexOf(';');
      if (semi < 0) return null;
      var g0 = parseInt(s.slice(1, semi), 10);
      if (isFinite(g0) && g0 > 0) events.push({ level: false, usec: g0 * 1000 });
      s = s.slice(semi + 1);
    }
    var segs = s.split(';');
    for (var i = 0; i < segs.length; i++) {
      var seg = segs[i].trim();
      if (!seg) continue;
      var gapMs = 0;
      var at = seg.indexOf('@');
      if (at > 0) {
        var gv = parseInt(seg.slice(at + 1), 10);
        if (isFinite(gv) && gv > 0) gapMs = gv;
        seg = seg.slice(0, at).trim();
      }
      var ev = signalSegmentToEvents(seg);
      if (!ev) return null;
      events = events.concat(ev);
      if (gapMs > 0) events.push({ level: false, usec: gapMs * 1000 });
    }
    if (!events.length) return null;
    return { events: events, seqRepeat: seqRepeat };
  }

  // 事件流 -> SmartIR Raw 的值（**带符号的 JSON 数组字符串**）。
  // ① 先合并相邻同电平（带符号的列表里不允许出现两个连续的负号）；
  // ② 再按"亮=正、灭=负"落成数字 —— 于是"波形以熄灭开头"天然可表示，
  //    不需要以前那个"补一个 0 长度发光"的补丁（0 长度的发光本身也说不通）。
  function eventsToSignedRaw(parsed) {
    var merged = [];
    function push(level, usec) {
      if (!(usec > 0)) return;
      var last = merged[merged.length - 1];
      if (last && last.level === level) last.usec += usec;
      else merged.push({ level: level, usec: usec });
    }
    for (var r = 0; r < parsed.seqRepeat; r++) {
      if (r > 0) push(false, SMARTIR_BURST_GAP_US);
      for (var i = 0; i < parsed.events.length; i++) {
        push(parsed.events[i].level, parsed.events[i].usec);
      }
    }
    if (!merged.length) return null;
    var out = [];
    for (var k = 0; k < merged.length; k++) {
      var u = Math.round(merged[k].usec);
      out.push(merged[k].level ? u : -u);
    }
    return '[' + out.join(',') + ']';
  }

  // SmartIR 是「一个设备一个文件」，外壳字段三份都长一样，只有 commands 的**值**不同。
  // `commandsEncoding` 恒为 "Raw" —— 它在 SmartIR 里只是个"声明"，**不约束值的写法**；
  // 真正决定值怎么用的是 supportedController（见本节的背景说明）。
  function smartIREnvelope(dev, cmds, controller) {
    return {
      manufacturer: 'CT30W',
      supportedModels: [dev.name],
      supportedController: controller || 'MQTT',
      commandsEncoding: 'Raw',
      commands: cmds
    };
  }

  // 格式①：原样。把设备里存的串**一字不改**放进去。
  // ⚠️ 看起来"什么都没做"，但这正是关键：解析、转换、重编码统统不参与 ⇒
  //    任何协议都不会被我们搞坏，验收也只需要"字符串相等"。
  function deviceToSmartIRNative(dev, skipped) {
    var cmds = {};
    (dev.buttons || []).forEach(function (b) {
      var s = (b.signal || '').trim();
      if (s && s.length <= SMARTIR_MAX_PAYLOAD) {
        cmds[b.name] = s;
      } else {
        skipped.push(dev.name + ' / ' + b.name +
                     (s ? '（超长 ' + s.length + ' > ' + SMARTIR_MAX_PAYLOAD + '）' : '（空信号）'));
      }
    });
    if (!Object.keys(cmds).length) return null;
    return smartIREnvelope(dev, cmds, 'MQTT');
  }

  // 格式②：Raw 微秒波形（带符号 JSON 数组）。需要自己算 ⇒ 算不出来就跳过，不做兜底猜测。
  function deviceToSmartIRRaw(dev, skipped) {
    var cmds = {};
    (dev.buttons || []).forEach(function (b) {
      var parsed = signalStringToEvents(b.signal);
      var raw = parsed ? eventsToSignedRaw(parsed) : null;
      if (raw) cmds[b.name] = raw;
      else skipped.push(dev.name + ' / ' + b.name);
    });
    if (!Object.keys(cmds).length) return null;
    // 值是 JSON 数组 ⇒ 只对吃这种写法的控制器成立，所以外壳如实写 ESPHome
    // （它在 send() 里 json.loads 这个值）。要换 Xiaomi / LOOKin 自己改这一个词即可。
    return smartIREnvelope(dev, cmds, 'ESPHome');
  }

  // ── 「导出给 HA」的下拉菜单（v1.3.33）─────────────────────────────────
  // 以前这里是一个常驻的 <select> + 一个按钮：手机上 <select> 被 width:100% 撑成
  // 单独一行，白占高度。改成"一个按钮点开选格式"，格式在**点击那一刻**传进来，
  // 所以不再需要一个常驻控件去记"当前选的是哪种"。
  function closeHAExportMenu() {
    var menu = document.getElementById('ha-export-menu');
    var btn = document.getElementById('btn-device-export-ha');
    if (!menu || !btn) return;
    menu.classList.add('hidden');
    btn.setAttribute('aria-expanded', 'false');
  }

  function toggleHAExportMenu() {
    var menu = document.getElementById('ha-export-menu');
    var btn = document.getElementById('btn-device-export-ha');
    if (!menu || !btn) return;
    var willOpen = menu.classList.contains('hidden');
    menu.classList.toggle('hidden', !willOpen);
    btn.setAttribute('aria-expanded', willOpen ? 'true' : 'false');
  }

  // fmt 由菜单项带过来：'native'（原样）或 'raw'（带符号波形），默认 native。
  function exportDevicesForHA(fmt) {
    closeHAExportMenu();                 // 点完就收，别让它挂在页面上
    if (!state.devices.length) { showToast('还没有设备可导出'); return; }
    fmt = (fmt === 'raw') ? 'raw' : 'native';
    var convert = (fmt === 'raw') ? deviceToSmartIRRaw : deviceToSmartIRNative;
    var skipped = [];
    var files = [];
    state.devices.forEach(function (d) {
      var obj = convert(d, skipped);
      if (obj) files.push({ name: d.name, obj: obj });
    });
    if (!files.length) {
      showToast(fmt === 'raw' ? '没有可转换的信号（只支持时序码与 NEC）' : '没有可导出的按钮');
      return;
    }
    // SmartIR 是**一个设备一个文件**，所以这里逐个下载
    files.forEach(function (f, i) {
      setTimeout(function () {
        downloadJson(safeFileName(f.name) + '.json', f.obj);
      }, i * 400);
    });
    var msg = '已导出 ' + files.length + ' 个设备（' +
              (fmt === 'raw' ? 'Raw 微秒波形' : '原样') + '）';
    if (skipped.length) msg += '；' + skipped.length + ' 个按钮跳过（' + skipped.slice(0, 3).join('、') + (skipped.length > 3 ? ' 等' : '') + '）';
    showToast(msg);
  }

  function safeFileName(s) {
    return String(s || 'device').replace(/[\\/:*?"<>|]+/g, '_').slice(0, 40) || 'device';
  }

  // ── 设备 / 按钮配置（v1.3.10）───────────────────────────────────────────
  // 以前这份配置只存在浏览器的 localStorage 里：换一台手机、或清一次缓存就全没了，
  // 而界面上还写着"已保存到设备" —— 名不副实。现在以设备上的 /api/devices 为准，
  // localStorage 只当本地缓存；设备是旧固件（没有这个接口）时静默退回纯本地，不影响使用。
  var DEVICES_SCHEMA = 2;

  function cacheDevices(devices) {
    // 注意：这里必须是原生 localStorage 调用 —— 若写成 persistDevices 会无限递归
    // 包一层 { v, devices }：旧版本直接存数组，以后结构再改就能按版本迁移，
    // 而不是靠"读出来能不能当数组用"来猜。
    try {
      localStorage.setItem(STORAGE_DEVICES, JSON.stringify({ v: DEVICES_SCHEMA, devices: devices || [] }));
    } catch (e) {}
  }

  function readCachedDevices() {
    try {
      var raw = localStorage.getItem(STORAGE_DEVICES);
      if (!raw) return [];
      var obj = JSON.parse(raw);
      if (Array.isArray(obj)) return obj;                                  // 旧格式，直接用
      if (obj && obj.v === DEVICES_SCHEMA && Array.isArray(obj.devices)) return obj.devices;
      return [];                                                           // 版本不认识 ⇒ 不猜
    } catch (e) {
      return [];
    }
  }

  // 同步元数据：设备端**没有**版本号/时间戳，前端只能自己记"上次看到设备上是什么内容"，
  // 才能分辨"设备被别处改空了"和"设备上从来就没有过配置"这两件完全不同的事。
  //   seen=true  ⇒ 至少成功对过一次账（不管内容是不是空）
  //   hash       ⇒ 上次对账时设备上的内容指纹
  function devicesFingerprint(devices) {
    var s = JSON.stringify(devices || []);
    var h = 5381;
    for (var i = 0; i < s.length; i++) h = ((h << 5) + h + s.charCodeAt(i)) >>> 0;
    return h.toString(16);
  }

  function getDevicesMeta() {
    try {
      var raw = localStorage.getItem(STORAGE_DEVICES_META);
      return raw ? JSON.parse(raw) : null;
    } catch (e) { return null; }
  }

  function setDevicesMeta(devices) {
    try {
      localStorage.setItem(STORAGE_DEVICES_META, JSON.stringify({
        v: DEVICES_SCHEMA, seen: true, hash: devicesFingerprint(devices), at: Date.now()
      }));
    } catch (e) {}
  }

  // ── 设备配置的"分片提交"（v1.3.42）───────────────────────────────────────
  //
  // 为什么不能整份一次 POST：设备端 `ESP8266WebServer` 收 body 要**一次读完**
  // （`Parsing-impl.h:160-169`），读不满就直接丢连接 —— **连一个 4xx 都回不了**，
  // 浏览器只看到网络错误。实测设备刚学完指令时（最大连续块只剩 ~4.5 KB），
  // 1677 B 的配置**必然收不下**；而"学完就把新按钮加进设备"恰恰总发生在这一刻。
  // ⛔ 设备端调内存阈值救不了：那个闸在 handler 里，而失败发生在更早的"收 body"阶段。
  // ⇒ 按字节切片、逐片串行发；设备端追加写临时文件、末片校验后 rename。
  //
  // ⚠️ DEVICES_CHUNK_BYTES 必须与固件 `DEVICES_CHUNK_MAX_BYTES` 一致（512）。
  var DEVICES_CHUNK_BYTES = 512;

  // 字符串 → UTF-8 字节。⛔ 必须按**字节**切片：JS 的 String.indexOf/slice 走 UTF-16
  // code unit，会把非 BMP 字符（emoji）的代理对切开 ⇒ 单独的高代理编码成 U+FFFD ⇒ 数据损坏。
  function utf8Bytes(str) {
    if (typeof TextEncoder === 'function') return new TextEncoder().encode(str);
    // 兜底（无 TextEncoder 的老浏览器）：手工 UTF-8，同样按码点处理代理对
    var out = [], i, c, c2;
    for (i = 0; i < str.length; i++) {
      c = str.charCodeAt(i);
      if (c < 0x80) out.push(c);
      else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
      else if (c >= 0xd800 && c <= 0xdbff && i + 1 < str.length &&
               (c2 = str.charCodeAt(i + 1)) >= 0xdc00 && c2 <= 0xdfff) {
        c = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00);
        i++;
        out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
      } else {
        out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
      }
    }
    return new Uint8Array(out);
  }

  // 串行发完整份配置（设备端是追加写，**必须有序**）。失败时最多整份重来一次。
  function postDevicesChunked(devices) {
    var bytes = utf8Bytes(JSON.stringify(devices));
    var TOTAL = Math.max(1, Math.ceil(bytes.length / DEVICES_CHUNK_BYTES));

    function runOnce() {
      var seq = 0;
      function sendNext() {
        if (seq >= TOTAL) return Promise.resolve(true);
        var start = seq * DEVICES_CHUNK_BYTES;
        var slice = bytes.subarray(start, Math.min(start + DEVICES_CHUNK_BYTES, bytes.length));
        // len 是**总字节数**，设备端在末片拿它和文件实际大小对账（能发现"少发了一片"）
        var url = '/devices/chunk?seq=' + seq + '&total=' + TOTAL + '&len=' + bytes.length;
        return apiPost(url, slice, 'application/octet-stream')
          .then(function () { seq++; return sendNext(); });
      }
      return sendNext();
    }

    var triesLeft = 1;   // 第一次之外，额外允许整份重来一次
    function attempt() {
      return runOnce().catch(function (err) {
        // 只在"可重试"时重来：409 = 设备端丢了上传会话（上次半途中断）；无 status = 网络层。
        // 400/413/500/507 是确定性的，重来没用 —— 直接把原因交给上层（会弹 toast）。
        var retriable = !err || !err.status || err.status === 409;
        if (triesLeft > 0 && retriable) { triesLeft--; return attempt(); }
        throw err;
      });
    }
    return attempt();
  }

  function persistDevices(devices) {
    cacheDevices(devices);            // 本地先落一份，离线也能用
    if (!getBaseUrl()) {
      // 没填设备地址 ⇒ 这份配置确实只在本机（状态条要如实说）
      setDevicesSync('local');
      return Promise.resolve(false);
    }
    setDevicesSync('checking');       // 分片要发若干个请求，先把"正在与设备核对…"亮出来
    // 注意：getBaseUrl() 已含 /api，这里只能写 '/devices/chunk'（写成 '/api/devices/chunk'
    // 会拼成 /api/api/devices/chunk 而 404 —— 这个 bug 让"保存到设备"在 v1.3.10~v1.3.18 静默失效过）
    return postDevicesChunked(devices)
      .then(function () { setDevicesMeta(devices); setDevicesSync('ok'); return true; })
      .catch(function () { setDevicesSync('local'); return false; });
  }

  // ── 配置到底存在哪：顶部那条常驻状态 ─────────────────────────────────────
  // 正式存储是设备端 /devices.json，浏览器只有一份缓存。但用户从界面上看不出
  // "此刻进没进设备" —— 以前只在写入失败那一瞬间弹一次 toast（多半没看见），
  // 于是"设备离线时改的东西，等设备回来被覆盖"这种事情发生时用户完全无感。
  // 四种取值 + 各自的文案，见下。
  function devicesSyncText(mode) {
    switch (mode) {
      case 'ok':
        return '配置已存在设备上 —— 换手机、清缓存都不会丢。点一下重新核对';
      case 'local':
        return '⚠ 只存在这台浏览器里，还没写进设备 —— 换手机或清缓存会丢。点一下重试写入';
      case 'offline':
        // 区分"没配地址"和"配了但连不上"：前者用户该去设置页，后者等会儿重试就行
        return getBaseUrl()
          ? '连不上设备，显示的是本机缓存，内容可能与设备上不一致。点一下重试'
          : '还没填设备地址，配置不会进设备（只在浏览器里）。到「设置」里填一下';
      default:
        return '正在与设备核对…';
    }
  }

  function renderDevicesSync() {
    var el = document.getElementById('devices-sync');
    if (!el) return;
    var mode = state.devicesSync || 'checking';
    el.textContent = devicesSyncText(mode);
    el.className = 'sync-note' + (mode === 'ok' ? ' is-ok' : (mode === 'local' || mode === 'offline') ? ' is-warn' : '');
  }

  function setDevicesSync(mode) {
    state.devicesSync = mode;
    renderDevicesSync();
  }

  // 返回值是一个 Promise：调用方可以等"设备端数据真的应用完"再干活
  // （保存弹窗的下拉框就需要这个——见 openSaveToDeviceModal）
  //
  // ⚠️ 合并规则（这条以前是错的）：
  //   旧实现 = "设备端为空 且 本地非空 ⇒ 把本地推上去"。它把两件事混成了一件：
  //     ① 设备刚升级上来、还没有过配置 —— 应该推；
  //     ② 用户在**另一台手机**上把设备清空了 —— 应该采纳空状态。
  //   于是"A 手机删空 → 回 B 手机，本地缓存又把它推回设备"，删除白做，两台手机
  //   还会来回打架。现在靠 seen/hash 把这两种情况分开：只有"从没对过账"才推。
  function syncDevicesFromDevice(quiet) {
    if (!getBaseUrl()) { setDevicesSync('offline'); return Promise.resolve(); }
    return apiGet('/devices', { quiet: !!quiet }).then(function (res) {
      var remote = (res && res.data) || [];
      if (!Array.isArray(remote)) return;
      var meta = getDevicesMeta();

      if (devicesFingerprint(remote) === devicesFingerprint(state.devices)) {
        setDevicesMeta(remote);       // 内容一致，什么都不用动
        setDevicesSync('ok');
        return;
      }

      if (remote.length > 0) {
        // 设备端有内容 ⇒ 一切以它为准，换手机也能拿回配置
        state.devices = remote;
        cacheDevices(remote);
        setDevicesMeta(remote);
        setDevicesSync('ok');
        renderDevicesView();
        return;
      }

      if (!meta || !meta.seen) {
        // 从没成功对过账（首次用、或设备是旧固件一直没有这个接口）
        // ⇒ 多半是刚升级上来，把本地已有的配置推上去，别让老数据丢
        if (state.devices.length > 0) {
          persistDevices(state.devices);   // 成没成功由它自己更新状态条
        } else {
          setDevicesSync('ok');            // 两边都是空 ⇒ 本来就是一致的
        }
        return;
      }

      // 见过设备上有内容，现在空了 ⇒ 是在别处（另一台手机）删的，采纳空状态
      state.devices = [];
      cacheDevices([]);
      setDevicesMeta([]);
      setDevicesSync('ok');
      renderDevicesView();
    }).catch(function () {
      setDevicesSync('offline');   // 连不上 ⇒ 屏幕上那份是本机缓存，可能已过期
    });
  }

  function renderDevicesView() {
    renderDevicesSync();
    if (state.tab !== 'devices') return;
    if (state.devicePanelId != null) renderDevicePanel();
    else renderDevicesList();
  }

  function loadDevices(quiet) {
    state.devices = readCachedDevices();
    renderDevicesView();
    syncDevicesFromDevice(quiet);     // 设备端为准；失败就继续用本地缓存
  }

  // 需要"等设备端数据真的回来"再干活的场合（比如保存弹窗的下拉框）用这个
  function loadDevicesThen(cb) {
    state.devices = readCachedDevices();
    renderDevicesView();
    syncDevicesFromDevice(true).then(cb, cb);
  }

  // ── 「刚学到的指令还没进设备」（v1.3.43）────────────────────────────────
  // 学习结果自 v1.3.46 起会落盘（重启/断电/升级都不丢），但它只是**暂存** ——
  // 没写进 /devices.json 就不算正式记下。以前只能在「学习」页逐条点「保存到设备」；
  // 学完切到设备页就看不到那几条，很容易忘。这里补一个设备页入口。
  //
  // ⭐ 判据是「**信号内容**是否已在设备里」，不是「按钮名在不在」：
  //   · 逐条保存过 ⇒ 剩下没保存的那几条仍会提示（按名字判断做不到这点）
  //   · 重新学了同名指令、信号变了 ⇒ 仍会提示（纯按名字判断会漏掉）
  function pendingLearnedCommands() {
    var commands = state.learnResult || {};
    var inDevices = {};
    (state.devices || []).forEach(function (d) {
      (d.buttons || []).forEach(function (b) { inDevices[b.name] = b.signal; });
    });
    return Object.keys(commands).filter(function (name) {
      return inDevices[name] !== commands[name];
    });
  }

  // 该加进哪个设备：优先「学习时填的那个设备名」对应的设备；否则若只有一个设备就用它。
  // 都不满足 ⇒ 返回 null，提示条改成「去学习页逐条处理」（不做额外的选设备弹窗）。
  function learnHintTargetDevice() {
    var devices = state.devices || [];
    var want = lastLearnDeviceName();
    if (want) {
      var same = devices.filter(function (d) { return d.name === want; })[0];
      if (same) return same;
    }
    return devices.length === 1 ? devices[0] : null;
  }

  function renderDevicesLearnHint() {
    var box = document.getElementById('devices-learn-hint');
    if (!box) return;
    var pending = pendingLearnedCommands();
    if (!pending.length) { box.classList.add('hidden'); return; }

    var text = '刚学到 ' + pending.length + ' 条指令（' + pending.join('、') + '）还没进设备';
    var btn = document.getElementById('btn-devices-learn-add');
    var dev = learnHintTargetDevice();
    if (dev) {
      // 这一句是关键：很多人以为「学完了」就等于「存好了」
      text += ' —— 它们只在设备内存里，重启就丢';
      btn.textContent = '加入「' + dev.name + '」';
      btn.dataset.deviceId = String(dev.id);
    } else {
      text += '。到「学习」页逐条点「保存到设备」即可';
      btn.textContent = '去学习页';
      delete btn.dataset.deviceId;
    }
    document.getElementById('devices-learn-hint-text').textContent = text;
    box.classList.remove('hidden');
  }

  function addLearnedToDevice() {
    var btn = document.getElementById('btn-devices-learn-add');
    var id = btn.dataset.deviceId ? parseInt(btn.dataset.deviceId, 10) : null;
    if (!id) { switchTab('learn'); return; }     // 没法确定目标 ⇒ 去学习页逐条处理
    var pending = pendingLearnedCommands();
    if (!pending.length) { renderDevicesLearnHint(); return; }
    var commands = state.learnResult || {};

    var devices = state.devices.slice();
    var target = devices.filter(function (d) { return d.id === id; })[0];
    if (!target) { renderDevicesLearnHint(); return; }
    target.buttons = target.buttons || [];
    pending.forEach(function (name) {
      var exist = target.buttons.filter(function (b) { return b.name === name; })[0];
      // 同名就把信号换新（「重新学一遍」的语义），不同名才追加 —— 免得冒出两个同名按钮
      if (exist) exist.signal = commands[name];
      else target.buttons.push({ name: name, signal: commands[name] });
    });

    state.devices = devices;
    persistDevices(devices).then(function (ok) {
      showToast(ok ? ('已把 ' + pending.length + ' 条加入「' + target.name + '」')
                   : '已存到本地，但没写进设备（换手机会丢）');
      renderDevicesList();          // 里面会重算提示条（加进去的就不该再提示）
    });
  }

  function renderDevicesList() {
    // 提示条与设备列表相互独立：一台设备都没有时它也要显示（文案会引导去新建）
    renderDevicesLearnHint();
    var list = document.getElementById('devices-list');
    var empty = document.getElementById('devices-empty');
    var panel = document.getElementById('device-panel');
    var addForm = document.getElementById('device-add-form');
    panel.classList.add('hidden');
    addForm.classList.add('hidden');
    list.innerHTML = '';
    if (!state.devices.length) {
      empty.classList.remove('hidden');
      return;
    }
    empty.classList.add('hidden');
    state.devices.forEach(function (d) {
      var div = document.createElement('div');
      div.className = 'list-item device-item';
      // 以前这里没有任何删除入口：手滑建的设备会永久占位，还会被反复推给设备
      div.innerHTML =
        '<span class="name">' + escapeHtml(d.name) + '</span>' +
        '<span class="row-actions">' +
        '<button type="button" class="btn btn-secondary" data-act="open">打开</button>' +
        '<button type="button" class="btn btn-text" data-act="del">删除</button>' +
        '</span>';
      div.querySelector('[data-act="open"]').addEventListener('click', function () {
        state.devicePanelId = d.id;
        document.getElementById('device-panel-title').textContent = d.name;
        renderDevicePanel();
        document.getElementById('device-panel').classList.remove('hidden');
        list.classList.add('hidden');
        empty.classList.add('hidden');
      });
      div.querySelector('[data-act="del"]').addEventListener('click', function () {
        var n = (d.buttons && d.buttons.length) || 0;
        if (!confirm('删除设备「' + d.name + '」？' + (n ? '它下面的 ' + n + ' 个按钮也会一起删掉。' : ''))) return;
        state.devices = state.devices.filter(function (x) { return x.id !== d.id; });
        if (state.devicePanelId === d.id) state.devicePanelId = null;
        persistDevices(state.devices).then(function (ok) {
          showToast(ok ? '已删除设备' : '已从本地删除，但没写进设备（换手机会回来）');
        });
        renderDevicesList();
      });
      list.appendChild(div);
    });
  }

  function renderDevicePanel() {
    var dev = state.devices.find(function (d) { return d.id === state.devicePanelId; });
    var grid = document.getElementById('device-buttons');
    grid.innerHTML = '';
    if (!dev || !dev.buttons || !dev.buttons.length) {
      grid.textContent = '暂无按钮';
      return;
    }
    dev.buttons.forEach(function (btn, idx) {
      var cell = document.createElement('div');
      cell.className = 'device-button';
      // ⚠️ 删按钮以前只绑在 contextmenu（右键）上 —— 手机没有右键，等于**完全删不掉**。
      // 现在给每个格子加一个显式的 ×。
      cell.innerHTML =
        '<button type="button" class="btn-del" aria-label="删除按钮" title="删除按钮">×</button>' +
        '<span class="name">' + escapeHtml(btn.name) + '</span><span class="send-label">发送</span>';
      cell.querySelector('.btn-del').addEventListener('click', function (e) {
        e.preventDefault();
        e.stopPropagation();
        cancelLongPress();
        longPressActivated = true;      // 顺手压掉紧跟着的 click，别把信号发出去
        deleteDeviceButton(dev, idx, btn);
      });

      var longPressTimer;
      var longPressActivated = false;

      function startLongPress() {
        longPressTimer = setTimeout(function () {
          longPressTimer = null;
          longPressActivated = true;
          var newName = prompt('按钮名', btn.name);
          if (newName !== null) {
            var newSignal = prompt('信号（统一字符串）', btn.signal);
            if (newSignal !== null) {
              dev.buttons[idx] = { name: newName.trim() || btn.name, signal: (newSignal && newSignal.trim()) || btn.signal };
              state.devices = state.devices.slice();
              persistDevices(state.devices);
              renderDevicePanel();
            }
          }
        }, 600);
      }

      function cancelLongPress() {
        clearTimeout(longPressTimer);
        longPressTimer = null;
      }

      cell.addEventListener('mousedown', startLongPress);
      cell.addEventListener('mouseup', cancelLongPress);
      cell.addEventListener('mouseleave', cancelLongPress);
      cell.addEventListener('touchstart', startLongPress, { passive: true });
      cell.addEventListener('touchend', cancelLongPress, { passive: true });
      cell.addEventListener('touchcancel', cancelLongPress, { passive: true });
      cell.addEventListener('click', function (e) {
        if (e.target.closest && e.target.closest('.btn-del')) return;
        e.preventDefault();
        if (longPressActivated) {
          longPressActivated = false;
          return;
        }
        if (!needIp()) return;
        apiPost('/signals/send', btn.signal, 'text/plain').then(function () { showToast('已发送'); }).catch(function () {});
      });
      // 桌面端保留右键删除这个快捷方式，与 × 走同一条路径
      cell.addEventListener('contextmenu', function (e) {
        e.preventDefault();
        deleteDeviceButton(dev, idx, btn);
      });
      grid.appendChild(cell);
    });
  }

  function deleteDeviceButton(dev, idx, btn) {
    if (!confirm('删除按钮「' + btn.name + '」？')) return;
    dev.buttons.splice(idx, 1);
    state.devices = state.devices.slice();
    persistDevices(state.devices).then(function (ok) {
      if (!ok) showToast('已从本地删除，但没写进设备（换手机会回来）');
    });
    renderDevicePanel();
  }

  function escapeHtml(s) {
    if (s == null) return '';
    var div = document.createElement('div');
    div.textContent = s;
    return div.innerHTML;
  }

  function escapeAttr(s) {
    if (s == null) return '';
    return String(s).replace(/"/g, '&quot;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  function init() {
    bindRuleDrafts();     // 要在第一次 switchTab 之前：草稿同步得先挂上
    var tab = localStorage.getItem('ir_tab') || 'status';
    if (!document.querySelector('.panel[data-tab="' + tab + '"]')) tab = 'status';
    switchTab(tab);

    document.querySelectorAll('.tab').forEach(function (t) {
      t.addEventListener('click', function () {
        var tabName = t.dataset.tab;
        localStorage.setItem('ir_tab', tabName);
        switchTab(tabName);
      });
    });

    // role="tablist" 的键盘约定：←/→ 在页签间移动，Home/End 跳首尾。
    // 不实现的话键盘用户只能靠 Tab 一个个走过去。
    document.querySelector('.tab-bar').addEventListener('keydown', function (e) {
      var tabs = Array.prototype.slice.call(document.querySelectorAll('.tab'));
      var i = tabs.indexOf(document.activeElement);
      if (i < 0) return;
      var n = null;
      if (e.key === 'ArrowLeft') n = (i - 1 + tabs.length) % tabs.length;
      else if (e.key === 'ArrowRight') n = (i + 1) % tabs.length;
      else if (e.key === 'Home') n = 0;
      else if (e.key === 'End') n = tabs.length - 1;
      if (n === null) return;
      e.preventDefault();            // 别让页面跟着左右滚
      tabs[n].focus();
      localStorage.setItem('ir_tab', tabs[n].dataset.tab);
      switchTab(tabs[n].dataset.tab);
    });

    // 注意：这里不能直接把 loadStatus 当 handler —— 它会收到 MouseEvent 当第一个
    // 参数（quiet）而变成"静默"。手动点的刷新就该有反馈。
    document.getElementById('btn-refresh-status').addEventListener('click', function () { loadStatus(); });
    document.getElementById('btn-forward-toggle').addEventListener('click', toggleForwarding);
    document.getElementById('fw-md5').addEventListener('click', function () {
      var md5 = document.getElementById('fw-md5').textContent;
      if (md5 && md5 !== '—') copyToClipboard(md5);
    });

    document.getElementById('btn-refresh-recent').addEventListener('click', function () {
      loadRecent();
      loadForwardRecords();
    });
    document.getElementById('btn-live-recent').addEventListener('click', toggleLiveRecent);
    document.getElementById('btn-clear-recent').addEventListener('click', clearRecentSignals);
    document.getElementById('btn-clear-forward').addEventListener('click', clearForwardRecords);
    document.getElementById('btn-forward-test').addEventListener('click', forwardTest);

    document.getElementById('btn-learn-start').addEventListener('click', learnStart);
    document.getElementById('btn-learn-stop').addEventListener('click', learnStop);
    document.getElementById('btn-refresh-learn-result').addEventListener('click', function () { loadLearnResult(); });
    document.getElementById('btn-clear-learn-result').addEventListener('click', clearLearnResult);

    document.getElementById('btn-rule-new').addEventListener('click', function () {
      if (state.rules.length >= MAX_RULES) {
        showToast('规则已达上限 ' + MAX_RULES + ' 条，请先删除一条');
        return;
      }
      openNewRuleForm('', '');
    });
    document.getElementById('btn-rule-save').addEventListener('click', saveRule);
    document.getElementById('btn-rule-delete').addEventListener('click', deleteRule);
    document.getElementById('btn-rule-back').addEventListener('click', function () {
      state.ruleDetailId = null;
      ruleDraft.id = null;
      showRulesView();
    });
    document.getElementById('btn-rule-send-a').addEventListener('click', function () {
      var a = document.getElementById('rule-a').value.trim();
      if (!a) { showToast('A 为空'); return; }
      if (!needIp()) return;
      apiPost('/signals/send', a, 'text/plain').then(function () { showToast('已发送 A'); }).catch(function () {});
    });
    document.getElementById('btn-rule-send-b').addEventListener('click', function () {
      var b = document.getElementById('rule-b').value.trim();
      if (!b) { showToast('B 为空'); return; }
      if (!needIp()) return;
      apiPost('/signals/send', b, 'text/plain').then(function () { showToast('已发送 B'); }).catch(function () {});
    });

    document.getElementById('btn-new-rule-save').addEventListener('click', saveNewRule);
    document.getElementById('btn-capture-a').addEventListener('click', function () {
      startCapture('new-rule-a', 'btn-capture-a', 15);
    });
    document.getElementById('btn-capture-b').addEventListener('click', function () {
      switchTab('recent');
      if (!state.liveRecent) toggleLiveRecent();
      showToast('按下遥控器，在列表里对着要转发的信号点「设为 B」');
    });
    document.getElementById('btn-new-rule-cancel').addEventListener('click', closeNewRuleForm);

    document.getElementById('btn-device-add').addEventListener('click', function () {
      document.getElementById('device-add-form').classList.remove('hidden');
      document.getElementById('new-device-name').value = '';
    });
    document.getElementById('btn-new-device-save').addEventListener('click', function () {
      var name = document.getElementById('new-device-name').value.trim();
      if (!name) { showToast('请输入设备名称'); return; }
      var devices = state.devices.slice();
      // ⭐ 同名设备会让"保存到设备 / 一键加入学到的指令"分不清该往哪台加
      //   （用户实测踩过：三台同名「艾美特风扇」，四个按钮被拆在三处）。
      //   这里不禁用，但必须问一次。
      var dup = devices.filter(function (d) { return d.name === name; })[0];
      if (dup && !confirm('已经有一台「' + name + '」了。\n再建一台同名设备，"保存到设备"就分不清往哪台加（按钮会被拆到两处）。\n\n确定还要新建吗？')) return;
      var newId = devices.length ? Math.max.apply(null, devices.map(function (d) { return d.id; })) + 1 : 1;
      devices.push({ id: newId, name: name, buttons: [] });
      state.devices = devices;
      persistDevices(devices).then(function (ok) {
        showToast(ok ? '已添加设备' : '已添加到本地，但没写进设备（换手机会丢）');
      });
      document.getElementById('device-add-form').classList.add('hidden');
      renderDevicesList();
    });
    document.getElementById('btn-new-device-cancel').addEventListener('click', function () {
      document.getElementById('device-add-form').classList.add('hidden');
    });
    document.getElementById('btn-device-back').addEventListener('click', function () {
      state.devicePanelId = null;
      document.getElementById('device-panel').classList.add('hidden');
      document.getElementById('devices-list').classList.remove('hidden');
      renderDevicesList();
    });

    document.getElementById('btn-save-ip').addEventListener('click', function () {
      var ip = document.getElementById('setting-ip').value.trim().replace(/^https?:\/\//, '');
      localStorage.setItem(STORAGE_IP, ip);
      showToast('已保存');
      loadStatus();
    });
    document.getElementById('btn-check-connection').addEventListener('click', function () {
      if (!getBaseUrl()) { showToast('请先保存设备地址'); return; }
      apiGet('/status').then(function (data) {
        var d = data && data.data ? data.data : data;
        showToast('连接正常 · 固件 ' + (d.firmware || '?'));
      }).catch(function () {});
    });
    var savedIp = localStorage.getItem(STORAGE_IP);
    if (savedIp) document.getElementById('setting-ip').value = savedIp;

    // 上次学习的设备名回填到「学习」表单，省得每次重打（也让"保存到设备"的预填有据可依）
    try {
      var lastLearnDev = localStorage.getItem(STORAGE_LEARN_DEVICE);
      if (lastLearnDev) document.getElementById('learn-device-name').value = lastLearnDev;
    } catch (e) {}

    // 从设备打开的页面：说清"不用填地址"，并藏掉无意义的配网入口说明
    var originEl = document.getElementById('setting-origin');
    if (isServedFromDevice()) {
      originEl.textContent = '当前页面就是从设备打开的（' + location.origin + '），无需填写设备地址。';
    } else {
      originEl.textContent = '当前页面是本地文件，需要手填设备地址才能操作设备。';
    }
    // 配网页与安全模式页都由设备提供，本地文件打开时点了也没用 —— 拦下并说明
    function guardDeviceOnlyLink(el) {
      el.addEventListener('click', function (e) {
        if (!isServedFromDevice()) {
          e.preventDefault();
          showToast('该页面需在浏览器访问设备地址后打开');
        }
      });
    }
    guardDeviceOnlyLink(document.getElementById('link-setup'));
    guardDeviceOnlyLink(document.getElementById('banner-action'));

    document.getElementById('btn-save-to-device-ok').addEventListener('click', confirmSaveToDevice);
    document.getElementById('btn-save-to-device-cancel').addEventListener('click', closeSaveToDeviceModal);

    // 备份：导出 / 导入（纯前端，见上方"备份"一节）
    document.getElementById('btn-device-export').addEventListener('click', exportDevices);
    document.getElementById('btn-device-export-ha').addEventListener('click', function (e) {
      e.stopPropagation();   // 别让下面"点空白处收起"那条立刻把它又关掉
      toggleHAExportMenu();
    });
    document.querySelectorAll('#ha-export-menu .menu-item').forEach(function (item) {
      item.addEventListener('click', function () {
        exportDevicesForHA(item.getAttribute('data-fmt'));   // 内部自己会收菜单
      });
    });
    // 点页面任意处 / 按 Esc 收起。没这条菜单会一直挂在上面挡住设备列表。
    document.addEventListener('click', closeHAExportMenu);
    document.addEventListener('keydown', function (e) {
      if (e.key === 'Escape') closeHAExportMenu();
    });
    document.getElementById('btn-device-import').addEventListener('click', function () {
      pickJsonFile('device-import-file', importDevicesFrom);
    });
    document.getElementById('btn-rule-export').addEventListener('click', exportRules);
    document.getElementById('btn-rule-import').addEventListener('click', function () {
      pickJsonFile('rule-import-file', importRulesFrom);
    });

    // 「刚学到的指令还没进设备」→ 一键加入（没法确定目标设备时按钮会变成"去学习页"）
    document.getElementById('btn-devices-learn-add').addEventListener('click', addLearnedToDevice);

    // 状态条本身就是"重新与设备核对"的入口
    document.getElementById('devices-sync').addEventListener('click', function () {
      if (!getBaseUrl()) { showToast('还没填设备地址 —— 到「设置」里填一下'); return; }
      if (state.devicesSync === 'local') {
        // ⚠ 方向要反过来：本机有一份没能写进设备的配置，重试应该是"推上去"。
        // 走"拉下来"（syncDevicesFromDevice）会把本机这份覆盖掉 —— 设备端为空且
        // 见过内容时它的语义是"在别处删的"，于是采纳空状态。用户点这一下是为了
        // 保住自己的改动，不能反手把它删了。
        setDevicesSync('checking');
        persistDevices(state.devices);
        return;
      }
      setDevicesSync('checking');
      syncDevicesFromDevice(false);
    });

    renderDevicesSync();     // 首屏就把状态条画出来（切到别的页签时它看不见，但 DOM 是准的）

    document.getElementById('modal-overlay').addEventListener('click', function (e) {
      if (e.target === this) closeSaveToDeviceModal();
    });
  }

  // 页面切到后台就停掉轮询：手机把面板留在后台时，几秒一次的轮询会一直啃设备
  // 那点可怜的内存（启动后空闲仅约 3KB），最后把面板自己拖停。
  document.addEventListener('visibilitychange', function () {
    if (document.hidden) {
      stopStatusPoll();
      stopRecentPoll();
      stopLearnPoll();
      return;
    }
    state.reconnectDelay = 0;    // 回来了就别再按退避慢慢等，立刻补一次
    switchTab(state.tab);        // 回到前台：重新加载当前页并恢复它该有的轮询
  });

  init();
})();
