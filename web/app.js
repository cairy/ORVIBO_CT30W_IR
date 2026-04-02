(function () {
  'use strict';

  const STORAGE_IP = 'ir_device_ip';
  const STORAGE_DEVICES = 'ir_devices';
  const STORAGE_LAST_DEVICE = 'ir_last_device_id';

  const state = {
    tab: 'recent',
    recent: [],
    learnStatus: null,
    learnResult: null,
    learnPollTimer: null,
    rules: [],
    ruleDetailId: null,
    ruleNewVisible: false,
    rulePreFillB: null,
    rulePreFillA: null,
    devices: [],
    devicePanelId: null,
    deviceAddVisible: false,
    pendingSaveSignal: null,
    pendingAddRuleB: null,
    pendingAddRuleA: null
  };

  function getBaseUrl() {
    const ip = localStorage.getItem(STORAGE_IP);
    if (!ip || !ip.trim()) return null;
    return 'http://' + ip.trim().replace(/^https?:\/\//, '') + '/api';
  }

  function showToast(msg) {
    const el = document.getElementById('toast');
    el.textContent = msg;
    el.classList.remove('hidden');
    el.classList.add('show');
    clearTimeout(el._toastTimer);
    el._toastTimer = setTimeout(function () {
      el.classList.add('hidden');
      el.classList.remove('show');
    }, 2500);
  }

  function needIp() {
    if (!getBaseUrl()) {
      showToast('请先在设置中配置设备 IP');
      return false;
    }
    return true;
  }

  function handleApiError(err, data) {
    if (data && data.error) showToast(data.error);
    else showToast('请检查设备 IP 与网络');
  }

  function apiGet(path) {
    const base = getBaseUrl();
    if (!base) return Promise.reject(new Error('no ip'));
    return fetch(base + path, { method: 'GET' })
      .then(function (res) {
        return res.json().then(function (data) {
          if (!res.ok) {
            handleApiError(null, data);
            throw new Error(data && data.error ? data.error : 'request failed');
          }
          if (data && data.success === false) {
            handleApiError(null, data);
            throw new Error(data.error || 'request failed');
          }
          return data;
        });
      })
      .catch(function (err) {
        if (err.message !== 'no ip' && err.message !== 'request failed' && !err.message.startsWith('{')) {
          showToast('请检查设备 IP 与网络');
        }
        throw err;
      });
  }

  function apiPost(path, body, contentType) {
    const base = getBaseUrl();
    if (!base) return Promise.reject(new Error('no ip'));
    const isJson = contentType === 'application/json';
    const headers = {};
    if (isJson) headers['Content-Type'] = 'application/json';
    else if (typeof body === 'string') headers['Content-Type'] = 'text/plain;charset=utf-8';
    const opt = { method: 'POST', headers: headers };
    if (body != null) opt.body = isJson ? JSON.stringify(body) : (typeof body === 'string' ? body : '');
    return fetch(base + path, opt)
      .then(function (res) {
        return res.json().then(function (data) {
          if (!res.ok) {
            handleApiError(null, data);
            throw new Error(data && data.error ? data.error : 'request failed');
          }
          if (data && data.success === false) {
            handleApiError(null, data);
            throw new Error(data.error || 'request failed');
          }
          return data;
        });
      })
      .catch(function (err) {
        if (err.message !== 'no ip' && err.message !== 'request failed') {
          showToast('请检查设备 IP 与网络');
        }
        throw err;
      });
  }

  function apiPut(path, body) {
    const base = getBaseUrl();
    if (!base) return Promise.reject(new Error('no ip'));
    return fetch(base + path, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    })
      .then(function (res) {
        return res.json().then(function (data) {
          if (!res.ok) {
            handleApiError(null, data);
            throw new Error(data && data.error ? data.error : 'request failed');
          }
          if (data && data.success === false) {
            handleApiError(null, data);
            throw new Error(data.error || 'request failed');
          }
          return data;
        });
      })
      .catch(function (err) {
        if (err.message !== 'no ip' && err.message !== 'request failed') {
          showToast('请检查设备 IP 与网络');
        }
        throw err;
      });
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
    const d = sig.data || '0x0';
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

  function signalSummary(str) {
    if (!str || !str.length) return '';
    if (str.indexOf(';') >= 0) return '多信号序列';
    if (/^\d+,0x[0-9a-fA-F]+/.test(str)) {
      var m = str.match(/^(\d+),0x([0-9a-fA-F]+)/);
      var protocolNames = { 1: 'NEC', 2: 'RC5', 3: 'RC6', 4: 'Sony', 5: 'Samsung' };
      var name = protocolNames[parseInt(m[1], 10)] || '协议' + m[1];
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
    var d = new Date(typeof ts === 'number' ? ts : parseInt(ts, 10));
    var now = Date.now();
    var diff = now - d.getTime();
    if (diff < 60000) return '刚刚';
    if (diff < 3600000) return Math.floor(diff / 60000) + ' 分钟前';
    if (diff < 86400000) return Math.floor(diff / 3600000) + ' 小时前';
    return d.toLocaleDateString();
  }

  function switchTab(tab) {
    state.tab = tab;
    document.querySelectorAll('.panel').forEach(function (p) {
      p.classList.toggle('active', p.dataset.tab === tab);
    });
    document.querySelectorAll('.tab').forEach(function (t) {
      t.classList.toggle('active', t.dataset.tab === tab);
    });
    if (tab === 'recent') loadRecent();
    if (tab === 'learn') { if (state.learnStatus && state.learnStatus.isLearning) startLearnPoll(); }
    if (tab === 'rules') loadRules();
    if (tab === 'devices') loadDevices();
  }

  function loadRecent() {
    if (!needIp()) return;
    apiGet('/signals/recent').then(function (data) {
      var arr = data && data.data ? data.data : (Array.isArray(data) ? data : []);
      state.recent = arr;
      renderRecent();
    }).catch(function () {
      state.recent = [];
      renderRecent();
    });
  }

  function renderRecent() {
    var list = document.getElementById('recent-list');
    var empty = document.getElementById('recent-empty');
    list.innerHTML = '';
    if (!state.recent.length) {
      empty.classList.remove('hidden');
      return;
    }
    empty.classList.add('hidden');
    state.recent.forEach(function (item, index) {
      var str = sequenceToStr(item);
      var summary = signalSummary(str);
      var meta = formatTime(item.timestamp) + (item.matched ? ' · 已匹配' : '');
      var div = document.createElement('div');
      div.className = 'list-item';
      div.innerHTML =
        '<span class="meta">' + escapeHtml(meta) + '</span>' +
        '<span class="summary">' + escapeHtml(summary) + '</span>' +
        '<span class="signal-str" title="' + escapeAttr(str) + '">' + escapeHtml(str) + '</span>' +
        '<div class="actions">' +
        '<button type="button" class="btn btn-primary btn-send-recent">发送</button>' +
        '<button type="button" class="btn btn-secondary btn-copy-recent">复制</button>' +
        '<button type="button" class="btn btn-secondary btn-save-recent">保存到设备</button>' +
        '<button type="button" class="btn btn-secondary btn-add-rule-recent">加入规则</button>' +
        '<button type="button" class="btn btn-text btn-expand-recent">展开</button>' +
        '</div>';
      var signalStrEl = div.querySelector('.signal-str');
      var expandBtn = div.querySelector('.btn-expand-recent');
      expandBtn.addEventListener('click', function () {
        div.classList.toggle('expanded');
        expandBtn.textContent = div.classList.contains('expanded') ? '收起' : '展开';
      });
      div.querySelector('.btn-send-recent').addEventListener('click', function () {
        if (!needIp()) return;
        apiPost('/signals/send', { signals: item.signals, intervals: item.intervals || [] }, 'application/json')
          .then(function () { showToast('已发送'); }).catch(function () {});
      });
      div.querySelector('.btn-copy-recent').addEventListener('click', function () {
        copyToClipboard(str);
      });
      div.querySelector('.btn-save-recent').addEventListener('click', function () {
        state.pendingSaveSignal = str;
        openSaveToDeviceModal();
      });
      div.querySelector('.btn-add-rule-recent').addEventListener('click', function () {
        state.pendingAddRuleB = str;
        state.pendingAddRuleA = item.signals && item.signals.length === 1
          ? oneSignalToStr(item.signals[0]) : '';
        state.ruleNewVisible = true;
        state.ruleDetailId = null;
        switchTab('rules');
        showRulesView();
      });
      list.appendChild(div);
    });
  }

  function openSaveToDeviceModal() {
    loadDevices();
    var overlay = document.getElementById('modal-overlay');
    var sel = document.getElementById('save-device-select');
    var newName = document.getElementById('save-device-new-name');
    var btnName = document.getElementById('save-button-name');
    sel.innerHTML = '<option value="">-- 选择设备 --</option>';
    state.devices.forEach(function (d) {
      var opt = document.createElement('option');
      opt.value = d.id;
      opt.textContent = d.name;
      sel.appendChild(opt);
    });
    newName.value = '';
    btnName.value = '';
    overlay.classList.remove('hidden');
    document.getElementById('modal-save-device').classList.remove('hidden');
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
    var targetDevice;
    if (newName) {
      var newId = devices.length ? Math.max.apply(null, devices.map(function (d) { return d.id; })) + 1 : 1;
      targetDevice = { id: newId, name: newName, buttons: [] };
      devices.push(targetDevice);
    } else {
      var id = sel.value ? parseInt(sel.value, 10) : null;
      if (!id) { showToast('请选择设备或输入新设备名'); return; }
      targetDevice = devices.find(function (d) { return d.id === id; });
      if (!targetDevice) { showToast('请选择设备'); return; }
    }
    targetDevice.buttons = targetDevice.buttons || [];
    targetDevice.buttons.push({ name: btnName, signal: signal });
    state.devices = devices;
    localStorage.setItem(STORAGE_DEVICES, JSON.stringify(devices));
    showToast('已保存到设备');
    closeSaveToDeviceModal();
    if (state.tab === 'devices') renderDevicesList();
  }

  function learnStart() {
    if (!needIp()) return;
    var deviceName = document.getElementById('learn-device-name').value.trim();
    var commandNames = document.getElementById('learn-command-names').value.trim();
    var timeout = parseInt(document.getElementById('learn-timeout').value, 10) || 30;
    if (!commandNames) { showToast('请输入指令名'); return; }
    apiPost('/learn/start', {
      deviceName: deviceName || 'Learned',
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

  function startLearnPoll() {
    if (state.learnPollTimer) clearInterval(state.learnPollTimer);
    function poll() {
      if (!getBaseUrl()) return;
      apiGet('/learn/status').then(function (data) {
        var d = data && data.data ? data.data : data;
        state.learnStatus = d;
        var statusEl = document.getElementById('learn-status');
        statusEl.innerHTML = '学习中: ' + (d.currentCommand || '') + ' — ' + (d.progress || '');
        if (!d.isLearning) {
          clearInterval(state.learnPollTimer);
          state.learnPollTimer = null;
          document.getElementById('learn-form-box').classList.remove('hidden');
          document.getElementById('btn-learn-start').classList.remove('hidden');
          document.getElementById('btn-learn-stop').classList.add('hidden');
          loadLearnResult();
        }
      }).catch(function () {});
    }
    poll();
    state.learnPollTimer = setInterval(poll, 1500);
  }

  function learnStop() {
    if (!needIp()) return;
    apiPost('/learn/stop', {}, 'application/json').then(function () {
      showToast('已停止学习');
      if (state.learnPollTimer) {
        clearInterval(state.learnPollTimer);
        state.learnPollTimer = null;
      }
      document.getElementById('learn-form-box').classList.remove('hidden');
      document.getElementById('btn-learn-start').classList.remove('hidden');
      document.getElementById('btn-learn-stop').classList.add('hidden');
      document.getElementById('learn-status').classList.add('hidden');
    }).catch(function () {});
  }

  function loadLearnResult() {
    if (!needIp()) return;
    apiGet('/learn/result?format=string').then(function (data) {
      var obj = data && data.data ? data.data : data;
      state.learnResult = obj && obj.commands ? obj.commands : {};
      renderLearnResult();
      document.getElementById('learn-result-box').classList.remove('hidden');
    }).catch(function () {
      state.learnResult = {};
      renderLearnResult();
    });
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
        '<button type="button" class="btn btn-secondary btn-copy-learn">复制</button>' +
        '<button type="button" class="btn btn-secondary btn-save-learn">保存到设备</button>' +
        '<button type="button" class="btn btn-secondary btn-add-rule-learn">加入规则</button>' +
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
        openSaveToDeviceModal();
      });
      div.querySelector('.btn-add-rule-learn').addEventListener('click', function () {
        state.pendingAddRuleB = str;
        state.pendingAddRuleA = '';
        state.ruleNewVisible = true;
        state.ruleDetailId = null;
        switchTab('rules');
        showRulesView();
      });
      list.appendChild(div);
    });
  }

  function loadRules() {
    if (!needIp()) return;
    apiGet('/maps').then(function (data) {
      var arr = data && data.data ? data.data : (Array.isArray(data) ? data : []);
      state.rules = arr;
      showRulesView();
    }).catch(function () {
      state.rules = [];
      showRulesView();
    });
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
        document.getElementById('rule-name').value = rule.name || '';
        document.getElementById('rule-a').value = aSignalToStr(rule.aSignal);
        document.getElementById('rule-b').value = bSequenceToStr(rule.bSequence);
        return;
      }
    }

    if (state.ruleNewVisible) {
      listWrap.classList.add('hidden');
      empty.classList.add('hidden');
      newForm.classList.remove('hidden');
      document.getElementById('new-rule-name').value = '';
      document.getElementById('new-rule-a').value = state.pendingAddRuleA || '';
      document.getElementById('new-rule-b').value = state.pendingAddRuleB || '';
      return;
    }

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
    apiPut('/maps', { id: id, rule: { name: name || undefined, a: a, b: b } }).then(function () {
      showToast('已保存');
      state.ruleDetailId = null;
      loadRules();
    }).catch(function () {});
  }

  function deleteRule() {
    var id = state.ruleDetailId;
    if (id == null) return;
    if (!confirm('确定删除该规则？')) return;
    var rest = state.rules.filter(function (r) { return r.id !== id; });
    var rulesPayload = rest.map(function (r) {
      return {
        name: r.name,
        a: aSignalToStr(r.aSignal),
        b: bSequenceToStr(r.bSequence)
      };
    });
    apiPut('/maps', { rules: rulesPayload }).then(function () {
      showToast('已删除');
      state.ruleDetailId = null;
      loadRules();
    }).catch(function () {});
  }

  function saveNewRule() {
    var name = document.getElementById('new-rule-name').value.trim();
    var a = document.getElementById('new-rule-a').value.trim();
    var b = document.getElementById('new-rule-b').value.trim();
    if (!a || !b) { showToast('请填写 A 和 B'); return; }
    var nextId = state.rules.length ? Math.max.apply(null, state.rules.map(function (r) { return r.id; })) + 1 : 1;
    var newRule = { id: nextId, name: name || undefined, a: a, b: b };
    var rulesPayload = state.rules.map(function (r) {
      return { name: r.name, a: aSignalToStr(r.aSignal), b: bSequenceToStr(r.bSequence) };
    });
    rulesPayload.push({ name: newRule.name, a: newRule.a, b: newRule.b });
    apiPut('/maps', { rules: rulesPayload }).then(function () {
      showToast('已添加规则');
      state.ruleNewVisible = false;
      state.pendingAddRuleB = null;
      state.pendingAddRuleA = null;
      loadRules();
    }).catch(function () {});
  }

  function loadDevices() {
    try {
      var raw = localStorage.getItem(STORAGE_DEVICES);
      state.devices = raw ? JSON.parse(raw) : [];
    } catch (e) {
      state.devices = [];
    }
    if (state.tab === 'devices') {
      if (state.devicePanelId != null) renderDevicePanel();
      else renderDevicesList();
    }
  }

  function renderDevicesList() {
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
      div.innerHTML = '<span class="name">' + escapeHtml(d.name) + '</span><button type="button" class="btn btn-secondary">打开</button>';
      div.querySelector('.btn').addEventListener('click', function () {
        state.devicePanelId = d.id;
        document.getElementById('device-panel-title').textContent = d.name;
        renderDevicePanel();
        document.getElementById('device-panel').classList.remove('hidden');
        list.classList.add('hidden');
        empty.classList.add('hidden');
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
      cell.innerHTML = '<span class="name">' + escapeHtml(btn.name) + '</span><span class="send-label">发送</span>';
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
              localStorage.setItem(STORAGE_DEVICES, JSON.stringify(state.devices));
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
        e.preventDefault();
        if (longPressActivated) {
          longPressActivated = false;
          return;
        }
        if (!needIp()) return;
        apiPost('/signals/send', btn.signal, 'text/plain').then(function () { showToast('已发送'); }).catch(function () {});
      });
      cell.addEventListener('contextmenu', function (e) {
        e.preventDefault();
        if (confirm('删除该按钮？')) {
          dev.buttons.splice(idx, 1);
          state.devices = state.devices.slice();
          localStorage.setItem(STORAGE_DEVICES, JSON.stringify(state.devices));
          renderDevicePanel();
        }
      });
      grid.appendChild(cell);
    });
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
    var tab = localStorage.getItem('ir_tab') || 'recent';
    switchTab(tab);

    document.querySelectorAll('.tab').forEach(function (t) {
      t.addEventListener('click', function () {
        var tabName = t.dataset.tab;
        localStorage.setItem('ir_tab', tabName);
        switchTab(tabName);
      });
    });

    document.getElementById('btn-refresh-recent').addEventListener('click', loadRecent);

    document.getElementById('btn-learn-start').addEventListener('click', learnStart);
    document.getElementById('btn-learn-stop').addEventListener('click', learnStop);

    document.getElementById('btn-rule-new').addEventListener('click', function () {
      state.ruleNewVisible = true;
      state.ruleDetailId = null;
      state.pendingAddRuleB = null;
      state.pendingAddRuleA = null;
      showRulesView();
    });
    document.getElementById('btn-rule-save').addEventListener('click', saveRule);
    document.getElementById('btn-rule-delete').addEventListener('click', deleteRule);
    document.getElementById('btn-rule-back').addEventListener('click', function () {
      state.ruleDetailId = null;
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
    document.getElementById('btn-new-rule-cancel').addEventListener('click', function () {
      state.ruleNewVisible = false;
      state.pendingAddRuleB = null;
      state.pendingAddRuleA = null;
      showRulesView();
    });

    document.getElementById('btn-device-add').addEventListener('click', function () {
      document.getElementById('device-add-form').classList.remove('hidden');
      document.getElementById('new-device-name').value = '';
    });
    document.getElementById('btn-new-device-save').addEventListener('click', function () {
      var name = document.getElementById('new-device-name').value.trim();
      if (!name) { showToast('请输入设备名称'); return; }
      var devices = state.devices.slice();
      var newId = devices.length ? Math.max.apply(null, devices.map(function (d) { return d.id; })) + 1 : 1;
      devices.push({ id: newId, name: name, buttons: [] });
      state.devices = devices;
      localStorage.setItem(STORAGE_DEVICES, JSON.stringify(devices));
      document.getElementById('device-add-form').classList.add('hidden');
      showToast('已添加设备');
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
    });
    document.getElementById('btn-check-connection').addEventListener('click', function () {
      if (!getBaseUrl()) { showToast('请先保存设备 IP'); return; }
      apiGet('/status').then(function () {
        showToast('连接正常');
      }).catch(function () {});
    });
    var savedIp = localStorage.getItem(STORAGE_IP);
    if (savedIp) document.getElementById('setting-ip').value = savedIp;

    document.getElementById('btn-save-to-device-ok').addEventListener('click', confirmSaveToDevice);
    document.getElementById('btn-save-to-device-cancel').addEventListener('click', closeSaveToDeviceModal);

    document.getElementById('modal-overlay').addEventListener('click', function (e) {
      if (e.target === this) closeSaveToDeviceModal();
    });
  }

  init();
})();
