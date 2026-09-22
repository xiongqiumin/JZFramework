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

  // 单列表: 运行中与已完成的任务同列显示(不再分"当前任务/历史任务")
  var taskListEl = $('histList');
  var taskEmptyEl = $('histEmpty');
  var histHintEl = $('histHint');
  var clearTasksBtn = $('clearTasksBtn');

  var modelInfoEl = $('modelInfo');
  var pendingEl = $('pending');
  var pendingListEl = $('pendingList');
  var pendingCountEl = $('pendingCount');
  var clearPendingBtn = $('clearPendingBtn');
  var resetBtn = $('resetBtn');
  var clearHistoryBtn = $('clearHistoryBtn');

  // 终端运行面板(terminal_run 跑过的命令 / background 起的服务)
  var termListEl = $('termList');
  var termEmptyEl = $('termEmpty');
  var termHintEl = $('termHint');
  var termLogEl = $('termLog');

  tokenEl.value = token;

  // ------------------------------------------------------------------
  // 状态: 一次只跑一个任务(cur), 其余排队(queue)
  // ------------------------------------------------------------------
  var taskNo = 0;
  var cur = null;              // 当前运行中的任务
  var queue = [];              // 待提交队列
  var selected = null;         // 右侧选中的卡片
  var epoch = 0;               // UI 世代号: 清空历史后, 旧任务的回包不再落到界面

  // 左列表最多保留多少张卡片: 再早的从界面上摘掉(右侧对话记录仍在), 免得列表越滚越长
  var MAX_CARDS = 20;
  var droppedCards = 0;        // 已被摘掉的卡片数(只用于提示"共多少条 / 只显示最近几条")

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
    var n = taskListEl.children.length;
    taskEmptyEl.classList.toggle('hidden', n > 0);
    if (!n) { histHintEl.textContent = ''; return; }
    histHintEl.textContent = droppedCards
      ? ('共 ' + (n + droppedCards) + ' 条 · 只显示最近 ' + n + ' 条')
      : ('共 ' + n + ' 条');
  }

  // 列表只留最近 MAX_CARDS 张: 超出就把最老的(排在最后的)卡片摘掉
  function trimCards() {
    while (taskListEl.children.length > MAX_CARDS) {
      var last = taskListEl.lastElementChild;
      if (!last || (cur && cur.el === last))   // 运行中的卡片永远不摘
        break;
      if (selected && selected.el === last)
        selected = null;
      taskListEl.removeChild(last);
      droppedCards += 1;
    }
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
    else rCmd.appendChild(el('span', 'none', '无验证'));
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
    else if (ok) task.result.textContent = task.cmd ? '验证通过' : '通过（无验证）';
    else task.result.textContent = info.error ? ('错误: ' + info.error) : '未通过';

    var concl = ok
      ? (info.reply || (task.cmd ? '（空回复，验证脚本退出码 0）' : '（空回复）'))
      : (info.error || info.reply || '（无结论）');
    task.conclText.textContent = concl;
    task.conclText.title = concl;      // 卡片里只露几行, 完整结论看悬停
    task.conclText.className = 'text' + (ok ? ' ok' : '');
    task.concl.classList.remove('hidden');

    if (info.out) {
      task.out.textContent = info.out;
      task.out.classList.remove('hidden');
    }

    // 单列表: 卡片就地更新, 不再从"当前任务"搬运到"历史任务"
    if (selected === task) selected = null;
    refreshSide();
  }

  // 服务端当前状态快照(新连接且正有任务在跑时推送): 重建进行中任务的卡片与过程气泡。
  // 仅当本地还没有 cur 时才重建 —— 同一页面的 SSE 自动重连不应重复建卡(live 事件会继续续推)。
  function applyState(data) {
    if (!data || !data.busy) return;
    if (cur) return;

    taskNo += 1;
    var task = { no: taskNo, prompt: data.prompt || '(进行中的任务)', cmd: data.verify_cmd || '' };
    cur = task;
    endProc();
    resetProcKeep();
    makeCard(task);
    taskListEl.insertBefore(task.el, taskListEl.firstChild);
    trimCards();
    selectCard(task);
    refreshSide();
    setUiRunning(true);

    if (typeof data.round === 'number')
      task.rounds.textContent = '· 轮次 ' + data.round;

    if (data.think) addProc('think', String(data.think));
    if (data.reply) {
      addProc('say', String(data.reply));
      task.replyStreamed = true;      // 快照里的回复也算已经显示过
    }

    var tools = data.tools || [];
    for (var i = 0; i < tools.length; i++) {
      var t = tools[i];
      var bubble = makeToolBubble(t.name || 'tool', String(t.args || ''));
      messagesEl.appendChild(bubble);
      procEls.push(bubble);
      if (t.hasOwnProperty('ok'))
        fillToolResult(bubble, t.ok, t.result);   // 已有结果: 回填
      else
        bubble.__pending = true;                  // 还在等结果: 别被 trimProc 摘掉
    }
    trimProc();

    // 快照里带了上一轮验证输出就显示出来(重连前已经跑过验证的话, 否则这边看不到)
    if (data.last_verify_output) {
      task.out.textContent = data.last_verify_output;
      task.out.classList.remove('hidden');
    }
  }

  // ------------------------------------------------------------------
  // 最近任务(服务端记的那一摞): 刚打开网页时用它把历史卡片补回来。
  //   长程任务跑几十分钟很常见, 网页不会一直开着 —— 关掉期间跑完的任务不能凭空消失,
  //   所以服务端保留最近若干次请求 + 结论 + 验证输出, 连接上来就随 hello 帧送过来。
  //   只在本地还没有任何卡片时重建(SSE 自动重连时 live 事件本来就在往这边推)。
  // ------------------------------------------------------------------
  function fmtClock(ms) {
    var d = new Date(ms);
    function p(n) { return (n < 10 ? '0' : '') + n; }
    return p(d.getHours()) + ':' + p(d.getMinutes());
  }

  function applyRecent(list) {
    if (!Array.isArray(list) || !list.length) return;
    if (cur || taskListEl.children.length) return;

    for (var i = list.length - 1; i >= 0; i--) {   // 服务端最近在前: 倒着插, 新的才落在最上面
      var it = list[i];
      if (!it || it.status === 'running') continue;   // 在跑的那条由 state 快照建卡
      taskNo += 1;
      var task = { no: taskNo, prompt: it.prompt || '(任务)', cmd: it.verify_cmd || '' };
      makeCard(task);
      taskListEl.insertBefore(task.el, taskListEl.firstChild);
      finishCard(task, {
        status: it.status, rounds: it.verify_rounds, error: it.error,
        out: it.last_verify_output, reply: it.agent_reply
      });
      if (it.end_ms) task.rounds.textContent += ' · ' + fmtClock(it.end_ms);
    }
    trimCards();
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
    body.appendChild(el('div', 'p-cmd', task.cmd ? ('验证: ' + task.cmd) : '验证: 无验证'));
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
  // skipBody: 只落状态行(徽标/轮次/错误/验证输出), 不再贴一遍回复正文 ——
  //           回复已经作为过程气泡流式显示过时用(见 finishTask)。
  function addMessage(role, bodyHtml, meta, skipBody) {
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

      if (!skipBody) {
        var b2 = document.createElement('div');
        b2.className = 'bubble';
        b2.innerHTML = escapeHtml(bodyHtml) || '<i>（空回复）</i>';
        wrap.appendChild(b2);
      }

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

  // 发送按钮: 运行中不再显示"处理中…"。文案跟着输入框走 ——
  //   有文字 → "发送"(运行中提交会排进待提交队列); 没文字 → "停止"(中止当前任务)。
  function syncSendBtn() {
    var canStop = !!(cur && !inputEl.value.trim());
    var txt = canStop ? '停止' : '发送';
    if (sendBtn.textContent !== txt) sendBtn.textContent = txt;
    sendBtn.title = canStop
      ? '中止当前任务（待提交队列保留，会接着跑下一个）'
      : '发送（Enter）';
  }

  // "停止" = 只中止当前任务: 不动待提交队列, 当前任务收尾后 pump() 会接着跑下一个。
  function stopCurrent() {
    postReset('task');
  }

  function setUiRunning(run) {
    syncSendBtn();
  }

  // ------------------------------------------------------------------
  // "等待回复" 提示: 任务在跑的时候, 若连续 5 秒没有收到任何新消息(SSE 事件 / 回包),
  // 就在对话最末尾补一条等待提示; 一旦有新消息到达立刻隐藏并重新计时。
  // ------------------------------------------------------------------
  var WAIT_MS = 5000;
  var waitEl = null;
  var waitTimer = null;

  function hideWaiting() {
    if (waitTimer) { clearTimeout(waitTimer); waitTimer = null; }
    if (waitEl && waitEl.parentNode) waitEl.parentNode.removeChild(waitEl);
  }

  // 有新消息(或状态变化)就调一次: 撤掉提示, 还在跑就重新起 5s 计时
  function noteActivity() {
    hideWaiting();
    if (!cur) return;
    waitTimer = setTimeout(showWaiting, WAIT_MS);
  }

  function showWaiting() {
    waitTimer = null;
    if (!cur) return;
    if (!waitEl) waitEl = el('div', 'sys dim waiting', '正在等待回复…');
    messagesEl.appendChild(waitEl);     // 永远落在对话末尾
    scrollBottom();
  }

  // 收尾当前任务: 结论落聊天 + 卡片归档 + 复位按钮 + 泵下一个。
  //   /task 的回包 与 SSE 的 task_end 都走这里(提交方断连时只有后者会到 —— 服务端不会因为
  //   网页断开就停任务), 靠 cur 判空天然幂等: 先到的那条收尾, 后到的不再重复落一条回复。
  function finishTask(info) {
    var task = cur;
    cur = null;
    setUiRunning(false);
    if (task) {
      // 回复正文已经流式显示过(过程气泡里就是全文): 这里只补一行状态, 不再重贴一遍回复。
      addMessage('assistant', info.reply || '', {
        status: info.status, rounds: info.rounds, error: info.error, verifyOut: info.out
      }, !!task.replyStreamed);
      finishCard(task, info);
    }
    pump();
    noteActivity();          // 收尾后复位计时(已泵出新任务就重新计时, 否则撤掉提示)
  }

  function startTask(task) {
    var g = epoch;
    cur = task;
    endProc();                 // 新任务: 过程气泡另起
    resetProcKeep();           // 过程步数重新计数(旧任务的步骤留在界面上)
    dropPendingItem(task);
    makeCard(task);
    taskListEl.insertBefore(task.el, taskListEl.firstChild);
    trimCards();
    selectCard(task);
    refreshSide();
    setUiRunning(true);
    noteActivity();

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
        // 连接层面失败 ≠ 任务没跑起来: 服务端可能已经收到并在跑(它不因这条连接断开而停)。
        // 先问一次 /info: 还在跑就交给 SSE 的 task_end 收尾; 确实没在跑才是真的提交失败。
        isServerBusy(function (busy) {
          if (!busy) done({ status: 'failed', error: '请求失败: ' + e.message });
        });
      });

    function done(info) {
      if (g !== epoch) return;      // 已被"清空历史"作废: 不落聊天/不归档
      if (cur !== task) return;     // 已由 SSE 的 task_end 收尾: 不重复落一条
      finishTask(info);
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
    syncSendBtn();          // 输入框已清空: 运行中按钮切回"停止"
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

  // 服务端是否正忙(GET /info)。问不到(服务不在)就当不忙 —— 那种情况下确实也没任务在跑。
  function isServerBusy(cb) {
    fetch(infoUrl(), { method: 'GET' })
      .then(function (resp) { return resp.ok ? resp.json() : null; })
      .then(function (d) { cb(!!(d && d.busy)); })
      .catch(function () { cb(false); });
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
  var PROC_TAG = { 'think': '思考', 'say': '回复', 'tool': '工具', 'res': '结果', 'res fail': '结果' };
  var procEl = null;       // 正在追加的步骤行
  var procBody = null;     // 该步骤行的正文
  var procKind = '';
  var procEls = [];        // 当前任务的过程行, 按时间从旧到新
  var procOmitEl = null;   // "已省略 N 步" 标记
  var procOmitN = 0;

  // 工具气泡: id -> 等待结果回填的 DOM 节点(参考 ChatView.m_pendingToolIdx)。
  // 一个工具调用连同它的返回合并在同一个气泡里, 而不是分两行。
  var toolBubbles = {};
  var toolSeq = 0;         // tool_call 不带 id 时的兜底自增号

  function endProc() { procEl = null; procBody = null; procKind = ''; }

  function resetProcKeep() {
    procEls.length = 0;
    procOmitN = 0;
    toolBubbles = {};
    if (procOmitEl && procOmitEl.parentNode) procOmitEl.parentNode.removeChild(procOmitEl);
    procOmitEl = null;
  }

  function trimProc() {
    while (procEls.length > PROC_KEEP) {
      var oldEl = procEls[0];
      if (oldEl && oldEl.__pending) break;   // 还在等输出: 不摘(最旧的应是已收到结果的)
      procEls.shift();
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

  // 一行步骤: 标题(圆点 + 类型标签 [+ 工具名]) + 正文(可滚动)
  function makeProc(kind, name) {
    var row = el('div', 'proc ' + kind);

    var head = el('div', 'proc-head');
    head.appendChild(el('span', 'proc-dot'));
    head.appendChild(el('span', 'proc-tag', PROC_TAG[kind] || '步骤'));
    if (name) head.appendChild(el('span', 'proc-name', name));
    row.appendChild(head);

    var mono = (kind === 'tool' || kind === 'res' || kind === 'res fail');
    var body = el('div', 'proc-body' + (mono ? ' mono' : '') + ' hidden');
    row.appendChild(body);
    row.body = body;
    return row;
  }

  // 工具气泡: 调用参数与"输出"合并在同一张卡片里。参考 code_agent/ChatView 的
  // appendTool + appendToolResult —— 用 call.id 把结果回填到对应的工具卡。
  // 这样网页里一个工具调用连同它的返回只占一个气泡, 而不是分两行。
  function makeToolBubble(name, args) {
    var row = el('div', 'proc tool');

    var head = el('div', 'proc-head');
    head.appendChild(el('span', 'proc-dot'));
    head.appendChild(el('span', 'proc-tag', PROC_TAG.tool || '工具'));
    if (name) head.appendChild(el('span', 'proc-name', name));
    row.appendChild(head);

    var body = el('div', 'proc-body mono');
    body.textContent = args || '（无参数）';
    row.appendChild(body);

    // 结果段: 调用发出时先隐藏, tool_result 到达后回填并展开
    var resSec = el('div', 'proc-result hidden');
    resSec.appendChild(el('div', 'proc-res-label', '输出'));
    var resBody = el('div', 'proc-body mono');
    resSec.appendChild(resBody);
    row.appendChild(resSec);

    row.__resSec = resSec;
    row.__resBody = resBody;
    row.__pending = true;        // 收到结果前不要被 trimProc 摘掉
    return row;
  }

  function fillToolResult(row, ok, result) {
    row.__pending = false;
    row.classList.toggle('fail', !ok);
    row.__resSec.classList.remove('hidden');
    row.__resBody.classList.add(ok ? 'ok' : 'fail');
    var res = String(result == null ? '' : result);
    if (res.length > 800) res = res.slice(0, 800) + '…';
    row.__resBody.textContent = res || '（空返回）';
    scrollBottom();
  }

  // kind: think / say / tool / res / res fail；name 仅工具行使用
  //   回复(say)是这次任务的最终答案: 一个字都不截断, 结尾也不再重复贴一遍(收尾只补一行状态),
  //   所以这里必须显示全文。思考/工具等过程信息仍限长, 免得长任务把页面刷爆。
  var PROC_MAX = 2000;
  function addProc(kind, text, name) {
    if (!procEl || procKind !== kind) {
      procEl = makeProc(kind, name);
      procBody = procEl.body;
      messagesEl.appendChild(procEl);
      procKind = kind;
      procEls.push(procEl);
      trimProc();
    }
    var cur = procBody.textContent;
    var max = (kind === 'say') ? 0 : PROC_MAX;   // 0 = 不限长
    if (!max || cur.length < max) procBody.textContent = cur + (text || '');
    else if (cur.length < max + 100) procBody.textContent = cur + '…（已省略）';
    procBody.classList.toggle('hidden', procBody.textContent === '');
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
      // 连上来时服务端已经闲着, 而我这边还挂着一张"运行中"的卡片: 那是断连期间跑完的任务
      // (服务端不因网页断开而停任务) —— 用 hello 帧里带的最后一次结果给它收尾。
      if (cur && data.busy === false && data.last_result) {
        var lr = data.last_result;
        endProc();
        finishTask({
          status: lr.status, rounds: lr.verify_rounds, error: lr.error,
          out: lr.last_verify_output, reply: lr.agent_reply
        });
      }
      applyRecent(data.recent);   // 最近若干次请求/结果: 刚打开的网页据此补出历史卡片
      return;
    }

    if (type === 'state') {
      applyState(data);         // 新连接且正有任务在跑: 重建进行中任务的卡片与过程
      return;
    }

    if (type === 'thinking') {
      addProc('think', data.text || '');
      return;
    }

    if (type === 'reply') {
      // 回复已经在过程气泡里流式显示了(全文): 打个标记, 收尾时不再把同一段再贴一遍。
      if (cur && data.text) cur.replyStreamed = true;
      addProc('say', data.text || '');
      return;
    }

    if (type === 'tool_call') {
      endProc();                       // 工具调用单独成泡, 不与上一段思考/回复混行
      var targs = String(data.args || '');
      if (targs.length > 600) targs = targs.slice(0, 600) + '…';
      var bubble = makeToolBubble(data.name || 'tool', targs);
      messagesEl.appendChild(bubble);
      procEls.push(bubble);
      trimProc();
      var tid = data.id || ('t' + (++toolSeq));   // 无 id 时兜底自增
      toolBubbles[tid] = bubble;
      procKind = 'tool';               // 标记当前段为工具, 后续思考/回复另起一行
      return;
    }

    if (type === 'tool_result') {
      endProc();
      var rid = data.id;
      var bub = rid ? toolBubbles[rid] : null;
      if (bub && bub.parentNode) {     // 命中工具泡: 结果回填到同一个气泡
        fillToolResult(bub, data.ok, data.result);
        delete toolBubbles[rid];
      } else {
        // 没有可配对的工具调用(历史数据或不携带 id): 退化成独立结果行
        var res = String(data.result || '');
        if (res.length > 800) res = res.slice(0, 800) + '…';
        addProc(data.ok ? 'res' : 'res fail', res);
      }
      return;
    }

    if (type === 'task_start') {
      endProc();
      if (cur) {
        cur.rounds.textContent = '· 第 ' + (cur.roundN || 1) + ' 轮';
        // 没填验证命令就是"无验证": 跑完即通过, 不再有验证这一步
        cur.result.textContent = data.verify_cmd ? ('验证待跑: ' + data.verify_cmd) : '无验证';
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
      addSystem('⚠ 运行错误: ' + (data.text || '未知错误')
        + (data.retryable ? '（将自动重试）' : ''), 'err');
      if (cur) cur.result.textContent = data.retryable ? '出错，准备重试' : '运行时错误';
      return;
    }

    if (type === 'retry') {
      endProc();
      var rn = data.retry || 1, rm = data.max_retries || 0;
      addSystem('自动重试 ' + rn + '/' + rm + '：'
        + (data.delay_ms ? ('等待 ' + data.delay_ms + 'ms 后') : '')
        + '把错误反馈给 agent 继续任务', 'err');
      if (cur) cur.result.textContent = '自动重试 ' + rn + '/' + rm + ' 中';
      return;
    }

    if (type === 'task_end') {
      // 任务收尾: 与 /task 的回包同一份数据。提交方连接已断(网页刷新/关页面)时这是唯一的
      // 结果出口 —— 服务端不会因此停任务, 跑完照样推这一帧。
      endProc();
      finishTask({
        status: data.status, rounds: data.verify_rounds, error: data.error,
        out: data.last_verify_output, reply: data.agent_reply
      });
      refreshTerminal();     // 任务收尾会把后台命令清掉(terminal_cleanup), 面板跟着刷新
      return;
    }

    if (type === 'reset') {
      endProc();
      addSystem(data.cancelled ? '任务已被重置' : '会话已重置(对话历史已清空)');
      refreshTerminal();
      return;
    }

    // 终端相关的收尾(root 用别的页面停了命令 / 任务收尾清理了后台任务): 面板跟着刷新
    if (type === 'terminal_cleanup' || type === 'terminal_cancel') {
      refreshTerminal();
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
      if (!obj || !obj.type) return;
      onServerEvent(obj);
      noteActivity();          // 收到消息: 立刻隐藏"等待回复", 并按最新状态重新计时
    };
  }

  tokenEl.addEventListener('change', function () {
    connectEvents();
    termFail = 0;            // token 换了: 面板立刻重试(否则要等退避周期)
    refreshTerminal();
  });

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
      taskListEl.innerHTML = '';
      cur = null;
      queue.length = 0;
      pendingListEl.innerHTML = '';
      selected = null;
      droppedCards = 0;
      setUiRunning(false);
      noteActivity();          // 已无在跑任务: 顺带撤掉"等待回复"
      refreshSide();
      refreshPending();
    });
  });

  sendBtn.addEventListener('click', function () {
    if (cur && !inputEl.value.trim()) { stopCurrent(); return; }   // 空输入 + 运行中 = 停止
    submit();
  });
  inputEl.addEventListener('input', syncSendBtn);
  clearBtn.addEventListener('click', function () { messagesEl.innerHTML = ''; });
  clearTasksBtn.addEventListener('click', function () {
    // 只清列表: 运行中的任务卡片保留(否则任务还在跑卡片却没了)
    if (selected && selected.el) selected.el.classList.remove('sel');
    selected = null;
    taskListEl.innerHTML = '';
    if (cur && cur.el) taskListEl.appendChild(cur.el);
    droppedCards = 0;
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

  // ------------------------------------------------------------------
  // 终端运行面板: GET /terminal (列表) / GET /terminal/log (日志) / POST /terminal/cancel
  //   为什么要有它: agent 用 terminal_run(background=true) 起的服务, 以前只有模型自己能看
  //   —— 人想知道"服务在打什么"只能去问模型(烧 token)。这里给人一个出口, 代价是每 3 秒
  //   一次 /terminal 轮询(纯内存, 很便宜)。
  // ------------------------------------------------------------------
  var termOpenId = null;         // 当前展开日志的 run_id
  var termFail = 0;              // 连续失败次数(服务不在就别一直刷)
  var TERM_KEEP = 8;             // 面板上最多显示几条(服务端给 30 条)

  function termUrl(path) {
    var tk = tokenEl.value.trim() || token;
    return tk ? (path + (path.indexOf('?') >= 0 ? '&' : '?') + 'token=' + encodeURIComponent(tk)) : path;
  }

  function fmtDuration(ms) {
    var s = Math.max(0, Math.round(ms / 1000));
    if (s < 60) return s + 's';
    var m = Math.floor(s / 60);
    if (m < 60) return m + 'm' + (s % 60 ? (s % 60) + 's' : '');
    return Math.floor(m / 60) + 'h' + (m % 60 ? (m % 60) + 'm' : '');
  }

  function renderTerminal(list) {
    var runs = (list && list.runs) || [];
    termListEl.innerHTML = '';
    termEmptyEl.classList.toggle('hidden', runs.length > 0);
    termHintEl.textContent = list && list.running ? (list.running + ' 条在跑') : '';

    for (var i = 0; i < runs.length && i < TERM_KEEP; i++) {
      var r = runs[i];
      var li = el('li', 'term-item' + (r.status === 'running' ? ' running' : ''));
      if (r.run_id === termOpenId) li.classList.add('sel');

      var head = el('div', 'term-head');
      var badge = el('span', 'term-badge ' + (r.status === 'running' ? 'run' : (r.exit_code === 0 ? 'ok' : 'fail')),
        r.status === 'running' ? '运行中' : (r.exit_code === 0 ? '完成' : '结束'));
      head.appendChild(badge);
      if (r.background) head.appendChild(el('span', 'term-tag', '后台'));
      head.appendChild(el('span', 'term-time', fmtDuration(r.elapsed_ms)));
      var stop = el('button', 'ghost tiny term-stop', '停止');
      stop.title = '停掉这条命令(连它的子进程一起收)';
      stop.addEventListener('click', function (e) {
        e.stopPropagation();
        stopTerminalRun(this.getAttribute('data-rid'));
      });
      stop.setAttribute('data-rid', r.run_id);
      if (r.status !== 'running') stop.disabled = true;
      head.appendChild(stop);
      li.appendChild(head);

      li.appendChild(el('div', 'term-cmd', r.command || '(空命令)'));
      var meta = r.run_id + ' · ' + r.lines + ' 行';
      if (r.pid > 0 && r.status === 'running') meta += ' · pid ' + r.pid;
      li.appendChild(el('div', 'term-meta', meta));

      li.addEventListener('click', function () {
        var rid = this.getAttribute('data-rid');
        if (termOpenId === rid) { termOpenId = null; termLogEl.classList.add('hidden'); renderTerminal(lastTerminal); return; }
        loadTerminalLog(rid);
      });
      li.setAttribute('data-rid', r.run_id);
      termListEl.appendChild(li);
    }
  }

  var lastTerminal = null;

  var termTick = 0;

  function refreshTerminal() {
    termTick += 1;
    // 连续失败(服务不在/token 不对)时退到 15 秒一次, 别每 3 秒刷一堆失败请求; 成功了自动恢复
    if (termFail >= 3 && (termTick % 5) !== 0) return;
    fetch(termUrl('/terminal'), { method: 'GET' })
      .then(function (resp) { return resp.ok ? resp.json() : null; })
      .then(function (d) {
        termFail = 0;
        lastTerminal = d || { runs: [] };
        renderTerminal(lastTerminal);
        if (termOpenId) loadTerminalLog(termOpenId, true);   // 展开着就跟着刷新
      })
      .catch(function () { termFail += 1; });
  }

  function loadTerminalLog(runId, silent) {
    fetch(termUrl('/terminal/log?run_id=' + encodeURIComponent(runId) + '&tail=200'), { method: 'GET' })
      .then(function (resp) { return resp.json().catch(function () { return null; }); })
      .then(function (d) {
        if (!d || !d.run_id) {
          if (!silent) { termOpenId = null; termLogEl.classList.add('hidden'); }
          return;
        }
        termOpenId = d.run_id;
        var head = '[' + d.run_id + '] ' + (d.status === 'running' ? '运行中' : '结束')
          + '  ' + d.total_lines + ' 行' + (d.dropped ? ('(更早 ' + d.dropped + ' 行已丢弃)') : '');
        termLogEl.textContent = head + '\n' + (d.text || '(暂无输出)');
        termLogEl.classList.remove('hidden');
        renderTerminal(lastTerminal);
      })
      .catch(function () { /* 忽略 */ });
  }

  function stopTerminalRun(runId) {
    if (!runId) return;
    fetch(termUrl('/terminal/cancel?run_id=' + encodeURIComponent(runId)), { method: 'POST' })
      .then(function () { refreshTerminal(); })
      .catch(function () { /* 忽略 */ });
  }

  // 每 3 秒刷一次(hidden 时跳过: 不占后台标签页的 CPU)
  setInterval(function () {
    if (!document.hidden) refreshTerminal();
  }, 3000);

  probe();
  connectEvents();
  refreshSide();
  refreshPending();
  refreshTerminal();
  syncSendBtn();
  inputEl.focus();
})();
