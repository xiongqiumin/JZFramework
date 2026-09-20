(function () {
  'use strict';

  var params = new URLSearchParams(location.search);
  var token = params.get('token') || '';

  function $(id) { return document.getElementById(id); }
  var messagesEl = $('messages');
  var inputEl = $('input');
  var sendBtn = $('sendBtn');
  var clearBtn = $('clearBtn');
  var connDot = $('connDot');
  var connText = $('connText');
  var verifyCmdEl = $('verifyCmd');
  var maxRoundsEl = $('maxRounds');
  var tokenEl = $('token');
  var settingsBtn = $('settingsBtn');
  var settingsEl = $('settings');

  var curListEl = $('curList');
  var curEmptyEl = $('curEmpty');
  var curHintEl = $('curHint');
  var histListEl = $('histList');
  var histEmptyEl = $('histEmpty');
  var histHintEl = $('histHint');
  var clearTasksBtn = $('clearTasksBtn');

  var modelInfoEl = $('modelInfo');
  var pendingEl = $('pending');
  var pendingListEl = $('pendingList');
  var pendingCountEl = $('pendingCount');
  var clearPendingBtn = $('clearPendingBtn');
  var resetBtn = $('resetBtn');
  var clearHistoryBtn = $('clearHistoryBtn');

  tokenEl.value = token;

  // ------------------------------------------------------------------
  // 状态: 一次只跑一个任务(cur), 其余排队(queue)
  // ------------------------------------------------------------------
  var taskNo = 0;
  var cur = null;              // 当前运行中的任务
  var queue = [];              // 待提交队列
  var selected = null;         // 右侧选中的卡片
  var epoch = 0;               // UI 世代号: 清空历史后, 旧任务的回包不再落到界面

  function escapeHtml(s) {
    return String(s == null ? '' : s)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
  }

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }

  function row(parent, key, valueCls, valueText) {
    var r = el('div', 'row');
    r.appendChild(el('span', 'k', key));
    r.appendChild(el('span', valueCls, valueText));
    parent.appendChild(r);
    return r;
  }

  function scrollBottom() {
    messagesEl.scrollTop = messagesEl.scrollHeight;
  }

  function setConn(ok) {
    connDot.className = 'dot ' + (ok ? 'on' : 'off');
    connText.textContent = ok ? '已连接' : '无法连接服务器';
  }

  // ------------------------------------------------------------------
  // 右侧任务卡片
  // ------------------------------------------------------------------
  function selectCard(card) {
    if (selected && selected.el) selected.el.classList.remove('sel');
    selected = card;
    if (card) card.el.classList.add('sel');
  }

  function refreshSide() {
    curEmptyEl.classList.toggle('hidden', curListEl.children.length > 0);
    histEmptyEl.classList.toggle('hidden', histListEl.children.length > 0);
    curHintEl.textContent = curListEl.children.length ? '运行中 ' + curListEl.children.length : '';
    histHintEl.textContent = histListEl.children.length ? '共 ' + histListEl.children.length + ' 条' : '';
  }

  function focusMsg(task) {
    if (task && task.msg) task.msg.scrollIntoView({ behavior: 'smooth', block: 'center' });
  }

  function makeCard(task) {
    var li = el('li', 'task-card');
    task.el = li;

    var top = el('div', 'task-top');
    top.appendChild(el('span', 'task-no', '#' + task.no));
    task.badge = el('span', 'task-badge run', '运行中');
    top.appendChild(task.badge);
    task.rounds = el('span', 'task-rounds', '');
    top.appendChild(task.rounds);
    li.appendChild(top);

    var t = el('div', 'task-title', task.prompt);
    t.title = task.prompt;
    li.appendChild(t);

    var v = el('div', 'task-verify');
    var rCmd = el('div', 'row');
    rCmd.appendChild(el('span', 'k', '验证:'));
    if (task.cmd) rCmd.appendChild(el('span', 'cmd', task.cmd));
    else rCmd.appendChild(el('span', 'none', '未设置'));
    v.appendChild(rCmd);

    var rRes = el('div', 'row');
    rRes.appendChild(el('span', 'k', '结果:'));
    task.result = el('span', 'task-res', '等待返回…');
    rRes.appendChild(task.result);
    v.appendChild(rRes);

    task.concl = el('div', 'task-concl hidden');
    var rConcl = el('div', 'row');
    rConcl.appendChild(el('span', 'k', '结论:'));
    task.conclText = el('span', 'text', '');
    rConcl.appendChild(task.conclText);
    task.concl.appendChild(rConcl);
    v.appendChild(task.concl);

    task.out = el('pre', 'task-out hidden');
    v.appendChild(task.out);

    li.appendChild(v);
    li.addEventListener('click', function () {
      selectCard(task);
      focusMsg(task);
    });
    return li;
  }

  // 完成: 回填结论 + 归档到历史区
  function finishCard(task, info) {
    var ok = info.status === 'success';
    var cancelled = info.status === 'cancelled';
    task.badge.className = 'task-badge ' + (ok ? 'ok' : (cancelled ? 'cancel' : 'fail'));
    task.badge.textContent = ok ? '通过' : (cancelled ? '已取消' : '失败');
    task.el.classList.add(ok ? 'done-ok' : (cancelled ? 'done-cancel' : 'done-fail'));
    task.el.classList.remove('sel');

    if (typeof info.rounds === 'number') task.rounds.textContent = '· 轮次 ' + info.rounds;

    if (cancelled) task.result.textContent = '任务已被重置';
    else if (ok) task.result.textContent = info.out ? '验证通过' : '任务完成';
    else task.result.textContent = info.error ? ('错误: ' + info.error) : '未通过';

    var concl = ok
      ? (info.reply || '（空回复，验证脚本退出码 0）')
      : (info.error || info.reply || '（无结论）');
    task.conclText.textContent = concl;
    task.conclText.className = 'text' + (ok ? ' ok' : '');
    task.concl.classList.remove('hidden');

    if (info.out) {
      task.out.textContent = info.out;
      task.out.classList.remove('hidden');
    }

    // 归档: 当前任务区 → 历史任务区(最新在最上)
    if (task.el.parentNode === curListEl) curListEl.removeChild(task.el);
    histListEl.insertBefore(task.el, histListEl.firstChild);
    if (selected === task) selected = null;
    refreshSide();
  }

  // ------------------------------------------------------------------
  // 待提交队列
  // ------------------------------------------------------------------
  function refreshPending() {
    pendingCountEl.textContent = String(queue.length);
    pendingEl.classList.toggle('hidden', queue.length === 0);
  }

  function addPendingItem(task) {
    var li = el('li', 'pending-item');
    task.pendingEl = li;
    li.appendChild(el('span', 'p-no', '#' + task.no));

    var body = el('div', 'p-body');
    var t = el('div', 'p-text', task.prompt);
    t.title = task.prompt;
    body.appendChild(t);
    body.appendChild(el('div', 'p-cmd', task.cmd ? ('验证: ' + task.cmd) : '验证: 未设置'));
    li.appendChild(body);

    var del = el('button', 'ghost p-del', '移除');
    del.addEventListener('click', function () {
      var i = queue.indexOf(task);
      if (i >= 0) queue.splice(i, 1);
      if (task.pendingEl && task.pendingEl.parentNode) {
        task.pendingEl.parentNode.removeChild(task.pendingEl);
      }
      refreshPending();
    });
    li.appendChild(del);

    pendingListEl.appendChild(li);
    refreshPending();
  }

  function dropPendingItem(task) {
    if (task.pendingEl && task.pendingEl.parentNode) {
      task.pendingEl.parentNode.removeChild(task.pendingEl);
    }
    task.pendingEl = null;
    refreshPending();
  }

  // ------------------------------------------------------------------
  // 任务提交 / 队列泵
  // ------------------------------------------------------------------
  function addMessage(role, bodyHtml, meta) {
    var wrap = document.createElement('div');
    wrap.className = 'msg ' + role;

    if (role === 'user') {
      var b = document.createElement('div');
      b.className = 'bubble';
      b.innerHTML = escapeHtml(bodyHtml);
      wrap.appendChild(b);
    } else {
      var head = document.createElement('div');
      head.className = 'meta';
      if (meta) {
        if (meta.status) {
          var badge = document.createElement('span');
          var okc = meta.status === 'success';
          badge.className = 'badge ' + (okc ? 'ok' : 'fail');
          badge.textContent = okc ? '通过' : (meta.status === 'cancelled' ? '已取消' : '失败');
          head.appendChild(badge);
        }
        if (typeof meta.rounds === 'number') {
          var r = document.createElement('span');
          r.className = 'rounds';
          r.textContent = '· 验证轮次 ' + meta.rounds;
          head.appendChild(r);
        }
      }
      wrap.appendChild(head);

      var b2 = document.createElement('div');
      b2.className = 'bubble';
      b2.innerHTML = escapeHtml(bodyHtml) || '<i>（空回复）</i>';
      wrap.appendChild(b2);

      if (meta && meta.error) {
        var e = document.createElement('div');
        e.className = 'err';
        e.textContent = '错误: ' + meta.error;
        wrap.appendChild(e);
      }
      if (meta && meta.verifyOut) {
        var d = document.createElement('details');
        d.className = 'verifyout';
        var sum = document.createElement('summary');
        sum.textContent = '验证输出';
        var pre = document.createElement('pre');
        pre.textContent = meta.verifyOut;
        d.appendChild(sum);
        d.appendChild(pre);
        wrap.appendChild(d);
      }
    }
    messagesEl.appendChild(wrap);
    scrollBottom();
    return wrap;
  }

  function setUiRunning(run) {
    sendBtn.textContent = run ? '处理中…' : '发送';
  }

  function startTask(task) {
    var g = epoch;
    cur = task;
    endProc();                 // 新任务: 过程气泡另起
    resetProcKeep();           // 过程步数重新计数(旧任务的步骤留在界面上)
    dropPendingItem(task);
    makeCard(task);
    curListEl.insertBefore(task.el, curListEl.firstChild);
    selectCard(task);
    refreshSide();
    setUiRunning(true);

    var req = { prompt: task.prompt };
    if (task.cmd) req.verify_cmd = task.cmd;
    var mr = parseInt(maxRoundsEl.value, 10);
    if (!isNaN(mr) && mr > 0) req.max_verify_rounds = mr;

    var tk = tokenEl.value.trim() || token;
    var url = tk ? ('/task?token=' + encodeURIComponent(tk)) : '/task';

    fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(req)
    })
      .then(function (resp) {
        return resp.json().catch(function () { return {}; }).then(function (data) {
          var info = resp.ok
            ? {
              status: data.status, rounds: data.verify_rounds, error: data.error,
              out: data.last_verify_output, reply: data.agent_reply
            }
            : { status: 'failed', error: (data && data.error) || ('HTTP ' + resp.status) };
          done(info);
        });
      })
      .catch(function (e) {
        done({ status: 'failed', error: '请求失败: ' + e.message });
      });

    function done(info) {
      if (g !== epoch) return;   // 已被"清空历史"作废: 不落聊天/不归档
      addMessage('assistant', info.reply || '', {
        status: info.status, rounds: info.rounds, error: info.error, verifyOut: info.out
      });
      if (cur === task) {
        finishCard(task, info);
        cur = null;
      }
      setUiRunning(false);
      pump();
    }
  }

  function pump() {
    if (cur || queue.length === 0) return;
    startTask(queue.shift());
  }

  function submit() {
    var prompt = inputEl.value.trim();
    if (!prompt) return;

    var cmd = verifyCmdEl.value.trim();
    taskNo += 1;
    var task = { no: taskNo, prompt: prompt, cmd: cmd };

    task.msg = addMessage('user', prompt);
    inputEl.value = '';
    inputEl.focus();

    if (cur) {
      // 正在运行 → 排入待提交队列
      queue.push(task);
      addPendingItem(task);
      return;
    }
    startTask(task);
  }

  // ------------------------------------------------------------------
  // 顶栏运行信息: GET /info(模型地址 / 模型名 / 工作区)
  //   不含 token —— 服务端 makeInfo() 刻意不吐 modelToken
  // ------------------------------------------------------------------
  function infoUrl() {
    var tk = tokenEl.value.trim() || token;
    return tk ? ('/info?token=' + encodeURIComponent(tk)) : '/info';
  }

  function applyInfo(data) {
    if (!data || typeof data !== 'object') return;
    var url = (data.modelUrl || '').replace(/\/+$/, '');
    var name = data.modelName || '';
    modelInfoEl.textContent = '模型: ' + (url ? (name ? name + ' @ ' + url : url) : '未配置');

    var tips = [];
    if (name) tips.push('模型名: ' + name);
    if (url) tips.push('地址: ' + url);
    if (data.rootDir) tips.push('工作区: ' + data.rootDir);
    if (data.version) tips.push('版本: ' + (data.agent || 'agent_cli') + ' ' + data.version);
    modelInfoEl.title = tips.join('\n') || '未获取运行信息';
  }

  function fetchInfo() {
    fetch(infoUrl(), { method: 'GET' })
      .then(function (resp) { return resp.ok ? resp.json() : null; })
      .then(applyInfo)
      .catch(function () { /* 保持占位 */ });
  }

  // GET /scripts 只当连通性探针(下拉选脚本已移除, 验证命令手填一行)
  function probe() {
    fetch('/scripts', { method: 'GET' })
      .then(function (resp) { setConn(resp.ok || resp.status === 401); })
      .catch(function () { setConn(false); });
    fetchInfo();
  }

  // ------------------------------------------------------------------
  // 实时事件流: GET /events (SSE) —— 报错/验证结果即时反馈
  // ------------------------------------------------------------------
  var evtSrc = null;

  // 过程气泡: 思考/回复是流式增量, 追加进同一条; 工具调用与结果各起一条。
  // 这样任务跑的过程中网页能看到 agent 在想什么/调什么工具, 而不是干等最后回包。
  // 只留最近 PROC_KEEP 步(用户输入 + 最近这些步骤), 更早的移除 —— 长任务不刷屏。
  var PROC_KEEP = 10;
  var procEl = null;
  var procKind = '';
  var procEls = [];        // 当前任务的过程气泡, 按时间从旧到新
  var procOmitEl = null;   // "已省略 N 步" 标记
  var procOmitN = 0;

  function endProc() { procEl = null; procKind = ''; }

  function resetProcKeep() {
    procEls.length = 0;
    procOmitN = 0;
    if (procOmitEl && procOmitEl.parentNode) procOmitEl.parentNode.removeChild(procOmitEl);
    procOmitEl = null;
  }

  function trimProc() {
    while (procEls.length > PROC_KEEP) {
      var oldEl = procEls.shift();
      if (oldEl.parentNode) oldEl.parentNode.removeChild(oldEl);
      procOmitN += 1;
    }
    if (procOmitN === 0) return;

    if (!procOmitEl) {
      procOmitEl = el('div', 'sys dim', '');
      var first = procEls[0];
      if (first && first.parentNode) messagesEl.insertBefore(procOmitEl, first);
      else messagesEl.appendChild(procOmitEl);
    }
    procOmitEl.textContent = '…（前面 ' + procOmitN + ' 步已省略）';
  }

  function addProc(kind, prefix, text) {
    if (!procEl || procKind !== kind) {
      procEl = document.createElement('div');
      procEl.className = 'proc ' + kind;
      procEl.textContent = prefix;
      messagesEl.appendChild(procEl);
      procKind = kind;
      procEls.push(procEl);
      trimProc();
    }
    if (procEl.textContent.length < 2000) procEl.textContent += text;
    else if (procEl.textContent.length < 2100) procEl.textContent += '…（已省略）';
    scrollBottom();
  }

  function addSystem(text, kind) {
    var d = document.createElement('div');
    d.className = 'sys' + (kind ? ' ' + kind : '');
    d.textContent = text;
    messagesEl.appendChild(d);
    scrollBottom();
    setTimeout(function () { d.classList.add('show'); }, 0);
    return d;
  }

  function onServerEvent(ev) {
    var type = ev.type, data = ev.data || {};

    if (type === 'hello') {
      setConn(true);
      applyInfo(data);          // hello 帧带同一份运行信息
      return;
    }

    if (type === 'thinking') {
      addProc('think', '思考: ', data.text || '');
      return;
    }

    if (type === 'reply') {
      addProc('say', '回复: ', data.text || '');
      return;
    }

    if (type === 'tool_call') {
      endProc();
      var args = String(data.args || '');
      if (args.length > 200) args = args.slice(0, 200) + '…';
      addProc('tool', '⚙ ' + (data.name || 'tool'), args ? (' ' + args) : '');
      return;
    }

    if (type === 'tool_result') {
      endProc();
      var res = String(data.result || '');
      if (res.length > 300) res = res.slice(0, 300) + '…';
      addProc('res' + (data.ok ? '' : ' fail'), data.ok ? '✔ ' : '✘ ', res);
      return;
    }

    if (type === 'task_start') {
      endProc();
      if (cur) {
        cur.rounds.textContent = '· 第 ' + (cur.roundN || 1) + ' 轮';
        if (data.verify_cmd) cur.result.textContent = '验证待跑: ' + data.verify_cmd;
      }
      return;
    }

    if (type === 'verify') {
      if (!cur) return;
      endProc();
      cur.roundN = (data.round || 0) + 1;
      cur.rounds.textContent = '· 轮次 ' + cur.roundN;
      if (data.output) {
        cur.out.textContent = data.output;
        cur.out.classList.remove('hidden');
      }
      cur.result.textContent = data.passed ? '验证通过' : '验证未通过(退出码 ' + data.exit_code + ')';
      addSystem(data.passed
        ? ('第 ' + cur.roundN + ' 轮验证通过')
        : ('第 ' + cur.roundN + ' 轮验证失败(退出码 ' + data.exit_code + ')，已把输出喂回 agent'));
      return;
    }

    if (type === 'error') {
      endProc();
      addSystem('⚠ 运行错误: ' + (data.text || '未知错误'), 'err');
      if (cur) cur.result.textContent = '运行时错误';
      return;
    }

    if (type === 'reset') {
      endProc();
      addSystem(data.cancelled ? '任务已被重置' : '会话已重置(对话历史已清空)');
      return;
    }
  }

  function connectEvents() {
    if (typeof EventSource === 'undefined') return;
    if (evtSrc) { evtSrc.close(); evtSrc = null; }

    var tk = tokenEl.value.trim() || token;
    var url = tk ? ('/events?token=' + encodeURIComponent(tk)) : '/events';
    var es = new EventSource(url);
    evtSrc = es;

    es.onopen = function () { setConn(true); };
    es.onerror = function () { setConn(false); };   // EventSource 会自动重连
    es.onmessage = function (e) {
      var obj;
      try { obj = JSON.parse(e.data); } catch (err) { return; }
      if (obj && obj.type) onServerEvent(obj);
    };
  }

  tokenEl.addEventListener('change', function () { connectEvents(); });

  // ------------------------------------------------------------------
  // 重置: POST /reset (后端调 JZAgent::reset())
  // ------------------------------------------------------------------
  function postReset(scope) {
    var tk = tokenEl.value.trim() || token;
    var url = tk ? ('/reset?token=' + encodeURIComponent(tk)) : '/reset';
    return fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ scope: scope })
    }).catch(function () { return null; });
  }

  function clearPendingQueue() {
    queue.length = 0;
    pendingListEl.innerHTML = '';
    refreshPending();
  }

  resetBtn.addEventListener('click', function () {
    clearPendingQueue();          // 待提交的一并丢弃(不再 pump)
    postReset('task');            // 在途 /task 请求会收到 status:"cancelled"
  });

  clearHistoryBtn.addEventListener('click', function () {
    var scope = cur ? 'task' : 'history';   // 有任务在跑就先中止它
    epoch += 1;                             // 作废在途任务的回包
    clearPendingQueue();
    postReset(scope).then(function () {
      messagesEl.innerHTML = '';
      histListEl.innerHTML = '';
      curListEl.innerHTML = '';
      cur = null;
      queue.length = 0;
      pendingListEl.innerHTML = '';
      selected = null;
      setUiRunning(false);
      refreshSide();
      refreshPending();
    });
  });

  sendBtn.addEventListener('click', submit);
  clearBtn.addEventListener('click', function () { messagesEl.innerHTML = ''; });
  clearTasksBtn.addEventListener('click', function () {
    histListEl.innerHTML = '';
    selected = null;
    refreshSide();
  });
  clearPendingBtn.addEventListener('click', function () {
    queue.length = 0;
    pendingListEl.innerHTML = '';
    refreshPending();
  });
  inputEl.addEventListener('keydown', function (e) {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      submit();
    }
  });
  settingsBtn.addEventListener('click', function () {
    settingsEl.classList.toggle('hidden');
  });

  // ------------------------------------------------------------------
  // 左右分栏拖动(左列表宽度): 拖动保存宽度到本地, 双击复位
  // ------------------------------------------------------------------
  (function setupSplitter() {
    var sideEl = document.querySelector('.side');
    var splitterEl = $('splitter');
    var bodyEl = document.querySelector('.body');
    if (!sideEl || !splitterEl || !bodyEl) return;

    var KEY = 'jz.agent_cli.sideWidth';
    var MIN_W = 200;            // 左下限
    var MIN_CHAT = 320;         // 右侧至少留这么宽

    function maxAllowed() {
      var avail = bodyEl.getBoundingClientRect().width;
      return Math.max(MIN_W, Math.min(640, avail - MIN_CHAT));
    }
    function clamp(v) { return Math.max(MIN_W, Math.min(v, maxAllowed())); }
    function apply(w) { sideEl.style.width = Math.round(w) + 'px'; }
    function store(w) {
      try { localStorage.setItem(KEY, String(Math.round(w))); } catch (e) { /* 隐私模式忽略 */ }
    }

    var saved = NaN;
    try { saved = parseInt(localStorage.getItem(KEY) || '', 10); } catch (e) { saved = NaN; }
    if (!isNaN(saved) && saved > 0) apply(clamp(saved));

    var dragging = false;
    splitterEl.addEventListener('pointerdown', function (e) {
      dragging = true;
      splitterEl.classList.add('active');
      document.body.classList.add('resizing');
      if (splitterEl.setPointerCapture) splitterEl.setPointerCapture(e.pointerId);
      e.preventDefault();
    });
    splitterEl.addEventListener('pointermove', function (e) {
      if (!dragging) return;
      apply(clamp(e.clientX - bodyEl.getBoundingClientRect().left));
    });
    function endDrag() {
      if (!dragging) return;
      dragging = false;
      splitterEl.classList.remove('active');
      document.body.classList.remove('resizing');
      store(sideEl.getBoundingClientRect().width);
    }
    splitterEl.addEventListener('pointerup', endDrag);
    splitterEl.addEventListener('pointercancel', endDrag);
    splitterEl.addEventListener('lostpointercapture', endDrag);
    splitterEl.addEventListener('dblclick', function () {
      try { localStorage.removeItem(KEY); } catch (e) { /* ignore */ }
      sideEl.style.width = '';        // 回 CSS 默认宽度(窄屏也跟着 300px)
    });
    window.addEventListener('resize', function () {
      apply(clamp(sideEl.getBoundingClientRect().width));
    });
  })();

  probe();
  connectEvents();
  refreshSide();
  refreshPending();
  inputEl.focus();
})();
