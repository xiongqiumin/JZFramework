/* ===========================================================================
   JZAgent 网页控制台 —— server(管理台) 与 agent_cli(自带 http) 共用这一份。

   两边后端都说 A2A v1.0, 所以前端只有一套数据形状:
     Task{ id, contextId, status{state,message,timestamp}, artifacts[], history[], metadata{} }
   页面:
     /            Agent 列表(只有 server 有; cli 的 / 直接是对话页)
     /agent?id=   单个 agent 的对话 + 任务日志(两边都有)
     /tasks       全部任务(server)

   约定:
     - 管理台的"任务日志"只列 origin=user(人下发的); agent 内部派活不算一条用户任务。
     - 验证命令不常驻: 收进「⚙ 设置」, 存本机 localStorage。
     - token 走 ?token= 或本机保存(cli 的 http.token 用得上; server 不需要)。
   ========================================================================== */

const PAGE = document.body.dataset.page || 'agents';
const api = (p) => '/api/' + p;
const QS = new URLSearchParams(location.search);

const S = {
  SUBMITTED: 'TASK_STATE_SUBMITTED',
  WORKING: 'TASK_STATE_WORKING',
  COMPLETED: 'TASK_STATE_COMPLETED',
  FAILED: 'TASK_STATE_FAILED',
  CANCELED: 'TASK_STATE_CANCELED',
  REJECTED: 'TASK_STATE_REJECTED',
  INPUT_REQUIRED: 'TASK_STATE_INPUT_REQUIRED',
  AUTH_REQUIRED: 'TASK_STATE_AUTH_REQUIRED',
  UNSPECIFIED: 'TASK_STATE_UNSPECIFIED'
};
const STATE_LABEL = {
  TASK_STATE_SUBMITTED: '已提交', TASK_STATE_WORKING: '执行中',
  TASK_STATE_COMPLETED: '已完成', TASK_STATE_FAILED: '失败',
  TASK_STATE_CANCELED: '已取消', TASK_STATE_REJECTED: '已拒绝',
  TASK_STATE_INPUT_REQUIRED: '需要补充输入', TASK_STATE_AUTH_REQUIRED: '需要认证',
  TASK_STATE_UNSPECIFIED: '结果未知'
};
const BADGE = {
  TASK_STATE_SUBMITTED: 'jz-badge--info', TASK_STATE_WORKING: 'jz-badge--info',
  TASK_STATE_COMPLETED: 'jz-badge--ok', TASK_STATE_FAILED: 'jz-badge--fail',
  TASK_STATE_CANCELED: 'jz-badge--warn', TASK_STATE_REJECTED: 'jz-badge--warn',
  TASK_STATE_INPUT_REQUIRED: 'jz-badge--warn', TASK_STATE_AUTH_REQUIRED: 'jz-badge--warn',
  TASK_STATE_UNSPECIFIED: 'jz-badge--warn'
};
const WORKING = [S.SUBMITTED, S.WORKING, S.INPUT_REQUIRED, S.AUTH_REQUIRED];

/* ---------------- 基础工具 ---------------- */

const TOKEN_KEY = 'jz.http.token';
const VERIFY_KEY = 'jz.http.verifyCmd';
(function rememberToken() {
  const t = QS.get('token');
  if (!t) return;
  try { localStorage.setItem(TOKEN_KEY, t); } catch (e) {}
})();
function lsGet(k) { try { return localStorage.getItem(k) || ''; } catch (e) { return ''; } }
function lsSet(k, v) { try { if (v) localStorage.setItem(k, v); else localStorage.removeItem(k); } catch (e) {} }
function withToken(url) {
  const t = lsGet(TOKEN_KEY);
  if (!t) return url;
  return url + (url.indexOf('?') >= 0 ? '&' : '?') + 'token=' + encodeURIComponent(t);
}

function showFatal(text) {
  const el = document.getElementById('msg');
  if (el) el.textContent = text;
  if (window.console) console.error(text);
}
window.addEventListener('error', (e) => showFatal('JS 错误: ' + (e.message || e.error)));
window.addEventListener('unhandledrejection', (e) => {
  const r = e.reason;
  showFatal('JS 异常: ' + ((r && r.message) ? r.message : r));
});

function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"]/g,
    c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}
function fmtTime(ms) {
  if (!ms) return '-';
  const d = new Date(Number(ms));
  const p = (n) => String(n).padStart(2, '0');
  return `${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}
function shortTime(ms) {
  if (!ms) return '';
  const d = new Date(Number(ms)), now = new Date();
  const p = (n) => String(n).padStart(2, '0');
  const hm = p(d.getHours()) + ':' + p(d.getMinutes());
  return d.toDateString() === now.toDateString() ? hm : p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' ' + hm;
}

/* ---- Task 访问器: 只走 A2A 形状 ---- */
const meta = (t) => (t && t.metadata) || {};
const stateOf = (t) => (t && t.status && t.status.state) || S.UNSPECIFIED;
const stateText = (t) => STATE_LABEL[stateOf(t)] || stateOf(t);
const isRunning = (t) => WORKING.indexOf(stateOf(t)) >= 0;
const isTerminal = (t) => !!t && !isRunning(t);
const taskId = (t) => (t && t.id) || '';
const taskAgent = (t) => meta(t).agentId || '';
const taskPrompt = (t) => meta(t).prompt || '';
/* 事件按页拿: 服务端只回最近 ~200 条(起点对齐到 user_msg), 更早的点"加载更多"再翻。
   翻过的部分缓存在本机, 轮询/刷新重新拉列表不会把它冲掉。 */
const AG_OLDER = {};        // taskId -> { events: [...], offset: n }
const taskEvents = (t) => {
  const id = t && taskId(t);
  const base = (meta(t) || {}).events || [];
  const older = id ? AG_OLDER[id] : null;
  return older ? older.events.concat(base) : base;
};
/* 还有多少条更早的事件没加载(0 = 到头了) */
function olderLeft(t) {
  const id = taskId(t);
  const older = AG_OLDER[id];
  const off = older ? older.offset : (meta(t) || {}).eventsOffset;
  return Number(off) || 0;
}
const taskOrigin = (t) => meta(t).origin || 'user';
const taskRounds = (t) => meta(t).rounds || 0;
function taskCost(t) {
  const m = meta(t);
  if (m.costMs > 0) return (m.costMs / 1000).toFixed(1) + 's';
  if (isRunning(t) && m.startMs) return Math.max(0, (Date.now() - m.startMs) / 1000).toFixed(0) + 's';
  return '-';
}
function partText(parts) { return (parts || []).map(p => p.text || '').join('\n'); }
function statusMessage(t) {
  const m = t && t.status && t.status.message;
  return m ? partText(m.parts) : '';
}
function artifactOf(t, id) {
  return ((t && t.artifacts) || []).filter(a => a && a.artifactId === id);
}
/* 只取"agent 回复"; 验证输出是另一条 artifact, 收在过程里看 */
function replyText(t) {
  return ((t && t.artifacts) || [])
    .filter(a => a && a.artifactId !== 'verify')
    .map(a => partText(a.parts)).filter(Boolean).join('\n');
}
function verifyText(t) {
  return artifactOf(t, 'verify').map(a => partText(a.parts)).filter(Boolean).join('\n');
}
function badge(t) {
  return `<span class="jz-badge ${BADGE[stateOf(t)] || ''}">${esc(stateText(t))}</span>`;
}

/* ---------------- HTTP ---------------- */
/* ---- 连接状态: 断了要在页面上说一声, 别让"发送"一直转圈 ---- */
/* ⚠ 别见到一次失败就报断线: SSE 在任务结束时会正常关流(EventSource 自己重连),
   轮询偶尔也会超时一次 —— 那一瞬间弹"连接已恢复"就是刷屏的根源。
   规则: 连续 2 次失败才算断; 断线超过 5s 才提示恢复(短暂抖动什么都不弹)。 */
const CONN = { lost: false, reason: '', since: 0, fails: 0 };
function setConn(ok, reason) {
  const bar = document.getElementById('connBar');
  if (ok) {
    CONN.fails = 0;
    if (!CONN.lost) return;
    const downMs = Date.now() - CONN.since;
    CONN.lost = false; CONN.reason = '';
    if (bar) bar.classList.add('hidden');
    if (downMs >= 5000 && window.JzUI) JzUI.toast('连接已恢复', 'ok');
    return;
  }
  const why = reason || '网络中断';
  // 浏览器 offline 事件是权威的, 一次就算断; 其它(请求失败/超时)要连续 2 次。
  // 注意: 本地连接(127.0.0.1)没有"网络抖动", 超时只可能是服务端处理慢 ——
  // 别把阈值调大来掩盖它, 该看服务端日志(slow 行)。
  CONN.fails = (why === '浏览器已离线') ? 2 : CONN.fails + 1;
  if (CONN.fails < 2) return;
  if (CONN.lost && CONN.reason === why) return;   // 同一个原因不重复刷
  CONN.lost = true; CONN.reason = why; CONN.since = Date.now();
  if (!bar) return;
  const t = document.getElementById('connText');
  if (t) t.textContent = '连接断开：' + why + '（' + hhmm(CONN.since) + '）· 正在自动重连…';
  bar.classList.remove('hidden');
}
/* 带超时的 fetch: 拿不到响应就 abort, 别让调用方永远等(默认 10s / POST 15s) */
async function fetchT(url, opts, timeoutMs) {
  const ctl = (typeof AbortController !== 'undefined') ? new AbortController() : null;
  const timer = ctl ? setTimeout(() => ctl.abort(), timeoutMs || 10000) : 0;
  try {
    return await fetch(url, ctl ? Object.assign({}, opts || {}, { signal: ctl.signal }) : opts);
  } finally {
    clearTimeout(timer);
  }
}
function netReason(e) {
  if (!e) return '网络中断';
  if (e.name === 'AbortError') return '请求超时';
  return '网络不可达';
}
async function getJson(url) {
  let r;
  try { r = await fetchT(withToken(url), null, 10000); }
  catch (e) {
    // 超时不等于断线: 本地连接没有抖动, 超时只说明服务端这一刻忙。
    // 真正的连接状态交给 SSE 下行判断(见 openEvents 的 EVT_LAST)。
    if (!e || e.name !== 'AbortError') setConn(false, netReason(e));
    throw e;
  }
  setConn(true);
  if (!r.ok) throw new Error('HTTP ' + r.status);   // 能拿到响应 = 连接是通的
  return r.json();
}
async function postJson(url, body, timeoutMs) {
  let r;
  try {
    r = await fetchT(withToken(url), {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body || {})
    }, timeoutMs || 15000);
  } catch (e) {
    if (!e || e.name !== 'AbortError') setConn(false, netReason(e));   // 超时不算断线
    throw e;
  }
  setConn(true);
  const j = await r.json().catch(() => ({}));
  return { ok: r.ok, json: j };
}
async function sendMessage({ agentId, prompt, verifyCmd, contextId, taskId, rootDir }) {
  const body = {
    agentId,
    message: { role: 'ROLE_USER', parts: [{ text: prompt }] },
    returnImmediately: true
  };
  if (verifyCmd) body.verifyCmd = verifyCmd;
  if (contextId) body.contextId = contextId;
  // 工作目录(网页上切换): 服务端记成这条任务的 project, cli 按它挑执行环境
  if (rootDir) { body.rootDir = rootDir; body.project = rootDir; }
  // 「接着说」: 复用这条任务的 taskId, 后端把这一轮追加进同一条记录
  if (taskId) body.taskId = taskId;
  const r = await postJson(api('tasks'), body);
  if (!r.ok) return { ok: false, error: r.json.error || '下发失败' };
  return { ok: true, id: r.json.id, contextId: r.json.contextId };
}
async function cancelTask(id) {
  const r = await postJson(api('tasks/cancel?id=' + encodeURIComponent(id)), {});
  return r.ok ? { ok: true, task: r.json }
              : { ok: false, error: r.json.error || '取消失败' };
}
/* 删历史记录: 在跑的会先取消再删(后端负责取消)。
   silent=true 用于批量场景(确认框由调用方先弹)。 */
async function deleteTaskRecords(ids, opts) {
  ids = (ids || []).filter(Boolean);
  if (!ids.length) return { ok: true, removed: 0 };
  if (!(opts && opts.silent)) {
    const running = !!(opts && opts.running);
    const head = running ? '任务还在执行，删除会先取消它。' : '';
    const text = ids.length === 1
      ? head + '删除这条任务记录？对话与执行过程都会从历史里消失，不可恢复。'
      : head + `删除这 ${ids.length} 条任务记录？不可恢复。`;
    const yes = window.JzUI
      ? await JzUI.confirm(text, { danger: true, title: '删除任务记录', okText: '删除' })
      : window.confirm(text);
    if (!yes) return { ok: false, cancelled: true };
  }
  const r = await postJson(api('tasks/delete'), { ids });
  if (!r.ok) return { ok: false, error: r.json.error || 'HTTP 删除失败' };
  return { ok: true, removed: r.json.removed || 0, skipped: r.json.skipped || [] };
}
/* 删完把本地列表同步掉: 该任务是被选中的话, 退回欢迎页 */
function dropTasks(ids) {
  const gone = {};
  ids.forEach(id => { gone[id] = 1; delete AG_OLDER[id]; });
  AG.tasks = AG.tasks.filter(t => !gone[taskId(t)]);
  if (gone[AG.sel]) { AG.sel = ''; AG.sig = ''; }
  renderSide(); renderHead(); renderTask(true, 'top');
  if (document.getElementById('fPrompt')) syncComposer();   // 删的正是那条在跑的: 输入框要立刻解锁
  const url = new URL(location.href);
  if (!AG.sel) url.searchParams.delete('task');
  history.replaceState(null, '', url);
}

/* ============================ /  Agent 列表 ============================ */

function agentRow(a) {
  const online = a.online !== false;
  const builtin = !!a.builtin;
  const state = !online ? '<span class="jz-badge">离线</span>'
    : (a.busy ? '<span class="jz-badge jz-badge--info">执行中</span>'
              : '<span class="jz-badge jz-badge--ok">空闲</span>');
  const tail = !online
    ? `最后在线 ${fmtTime(a.lastSeen)}`
    : (a.taskId ? esc(a.taskId) : (a.lastEventType ? esc(a.lastEventType) : '—'));
  return `<a class="conn${online ? '' : ' conn--off'}" href="?id=${encodeURIComponent(a.agentId)}">
    <span class="conn__mono">${esc((a.agentId || '?').slice(0, 2).toUpperCase())}</span>
    <span class="conn__main">
      <span class="conn__id">${esc(a.agentId)}
        ${builtin ? '<span class="jz-badge">本机</span>' : ''}</span>
      <span class="conn__sub">${esc(a.name || '')}${a.modelName ? ' · ' + esc(a.modelName) : ''}</span>
      <span class="conn__path">${esc(a.rootDir || '')}</span>
    </span>
    <span class="conn__side">${state}<span class="conn__task">${tail}</span></span>
  </a>`;
}

async function loadAgentsPage() {
  const metaEl = document.getElementById('meta');
  let s;
  try { s = await getJson(api('status')); }
  catch (e) { metaEl.textContent = '请求失败: ' + e; return; }

  const all = s.known || s.online || [];
  const selfs = all.filter(a => a.builtin);
  const clis = all.filter(a => !a.builtin);
  const online = clis.filter(a => a.online !== false).length;

  metaEl.textContent = (s.hub && s.hub.listening ? '控制面监听 ' + s.hub.port : '控制面关闭')
    + ' · 在线 ' + online + ' 台';

  // 逐项判空: 页面模板可能和脚本不是同一版(浏览器缓存/半更新), 别整页崩在这
  const selfHead = document.getElementById('selfHead');
  if (selfHead) selfHead.classList.toggle('hidden', !selfs.length);
  const selfList = document.getElementById('selfList');
  if (selfList) selfList.innerHTML = selfs.map(agentRow).join('');
  const cliList = document.getElementById('cliList');
  if (cliList) cliList.innerHTML = clis.length
    ? clis.map(agentRow).join('')
    : `<div class="conn conn--empty">还没有 agent cli 连过。<br>
       在 agent_cli 配置里填 <code>server.host</code> / <code>server.port</code>,
       或者打开本机的 <code>selfAgent.enabled</code>。</div>`;
}

/* ============================ /agent  对话 ============================ */

const AG = {
  id: '', kind: '', info: null, card: null, sel: '',
  tasks: [], es: null, watchId: '', poll: 0, sig: null, openMap: {},
  busy: false, termTimer: 0,
  ctx: ''            // 当前要接续的会话 id(选中历史任务时从它身上取; '' = 新会话)
};

const STEP_LABEL = {
  thinking: '思考', reply: '输出', tool_call: '工具', tool_result: '返回',
  verify: '验证', retry: '重试', error: '错误', terminal_cleanup: '终端'
};
function dotClass(t) {
  const s = stateOf(t).replace('TASK_STATE_', '');
  if (s === 'SUBMITTED' || s === 'WORKING') return 'run';
  if (s === 'COMPLETED') return 'ok';
  if (s === 'FAILED') return 'fail';
  return 'warn';
}
function stepLabel(type) { return STEP_LABEL[type] || type || 'event'; }
function jsonText(v) {
  if (v == null) return '';
  if (typeof v === 'string') return v;
  try { return JSON.stringify(v); } catch (e) { return String(v); }
}
function stepText(e) {
  const d = e && e.data, type = (e && e.type) || '';
  const pick = (k) => (d && typeof d === 'object') ? d[k] : undefined;
  let s = '';
  if (type === 'thinking' || type === 'reply') s = typeof d === 'string' ? d : (pick('text') || '');
  else if (type === 'user_msg') s = pick('text') || (typeof d === 'string' ? d : '');
  else if (type === 'tool_call') s = (pick('name') || 'tool') + (pick('args') ? ' ' + jsonText(pick('args')) : '');
  else if (type === 'tool_result') s = pick('result') != null ? jsonText(pick('result')) : jsonText(d);
  else if (type === 'verify') s = '第 ' + (pick('round') || 1) + ' 轮 · ' + (pick('cmd') || '验证');
  else if (type === 'retry') s = '第 ' + (pick('attempt') || '?') + ' 次重试 · ' + (pick('reason') || pick('error') || '');
  else if (type === 'error') s = pick('message') || pick('error') || pick('text') || jsonText(d);
  else s = jsonText(d);
  // ⚠ thinking/reply 是**流式增量**: 这里绝不能 trim/截断, 否则每个分片末尾的空格和换行
  //   会被吃掉, 拼起来就是"4个 node"、"| 文件位置"这种粘连文本。
  return String(s == null ? '' : s);
}
/* ===========================================================================
   一条任务 = 一个任务队列(一次提交)。队列内部有很多轮:
   思考 → 工具 → 回复 → 验证 → 重试…  按事件顺序展开成一段对话。
   =========================================================================== */
function groupEvents(t) {
  // start/task_start/task_end 不是对话内容; 老记录里还有叫 start 的事件
  const evs = taskEvents(t).filter(e => e && ['submitted', 'start', 'task_start', 'task_end'].indexOf(e.type) < 0);
  const out = [];
  for (let i = 0; i < evs.length; i++) {
    const e = evs[i], d = e.data || {}, type = e.type || '';
    const last = out[out.length - 1];
    if (type === 'thinking' || type === 'reply') {
      const txt = stepText(e);
      if (last && last.kind === type) last.text += txt;   // 流式增量: 同一轮拼成一段
      else out.push({ kind: type, text: txt });
      continue;
    }
    if (type === 'user_msg') {
      // 每一轮用户说了什么(同一条任务里"接着说"会追加多条)
      out.push({ kind: 'user', text: (d && d.text) || stepText(e) || '' });
      continue;
    }
    if (type === 'tool_call') {
      out.push({ kind: 'tool', name: (d && d.name) || 'tool',
                 args: (d && d.args) || '', result: null, ok: null });
      continue;
    }
    if (type === 'tool_result') {
      let target = null;
      for (let k = out.length - 1; k >= 0; k--) {
        if (out[k].kind === 'tool' && out[k].result === null) { target = out[k]; break; }
      }
      const res = (d && d.result != null) ? jsonText(d.result) : jsonText(d);
      if (target) { target.result = res; target.ok = !(d && d.ok === false); }
      else out.push({ kind: 'tool', name: '', args: '', result: res, ok: !(d && d.ok === false) });
      continue;
    }
    if (type === 'verify') {
      out.push({ kind: 'verify', round: (d && d.round) || 0, cmd: (d && d.cmd) || '',
                 passed: !!(d && d.passed), exit: (d && d.exit_code),
                 output: (d && d.output) || '' });
      continue;
    }
    out.push({ kind: 'sys', type: type, text: stepText(e) });
  }
  return out;
}
function blockHtml(b, key, open) {
  const attr = open ? ' open' : '';
  if (b.kind === 'thinking') {
    return `<details class="cs-think" data-open="${esc(key)}"${attr}>
      <summary>思考（${b.text.length} 字）</summary>
      <div class="cs-think__body">${esc(b.text)}</div></details>`;
  }
  if (b.kind === 'reply') {
    return `<div class="jz-msg jz-msg--agent"><div class="jz-bubble">${esc(b.text || '…')}</div></div>`;
  }
  if (b.kind === 'user') {
    return `<div class="jz-msg jz-msg--user"><div class="jz-bubble">${esc(b.text || '')}</div></div>`;
  }
  if (b.kind === 'tool') {
    const dot = b.result === null ? 'run' : (b.ok ? 'ok' : 'fail');
    const head = (b.name ? '<code>' + esc(b.name) + '</code>' : '<span class="muted">工具</span>')
      + (b.args ? ' <span class="muted">' + esc(String(b.args).slice(0, 140)) + '</span>' : '');
    return `<details class="cs-tool" data-open="${esc(key)}"${attr}>
      <summary><span class="cs-tool__dot ${dot}"></span>${head}</summary>
      <pre class="jz-log">${esc(b.result == null ? '（等待结果…）' : (b.result || '（空）'))}</pre>
    </details>`;
  }
  if (b.kind === 'verify') {
    const verdict = b.passed ? '通过' : ('未通过' + (b.exit != null ? '（退出码 ' + esc(String(b.exit)) + '）' : ''));
    return `<div class="jz-step cs-step--verify"><span class="jz-step__dot"></span>
        <span class="jz-step__tag">验证${b.round ? ' ' + b.round : ''}</span>
        <div class="jz-step__body">${esc((b.cmd || '验证命令') + ' · ' + verdict)}</div>
      </div>${b.output ? `<pre class="jz-log cs-verify-out">${esc(b.output)}</pre>` : ''}`;
  }
  const txt = String(b.text == null ? '' : b.text);
  return `<div class="jz-step cs-step--${esc(b.type || 'event')}">
      <span class="jz-step__dot"></span>
      <span class="jz-step__tag">${esc(stepLabel(b.type))}</span>
      <div class="jz-step__body">${esc(txt.length > 900 ? txt.slice(0, 900) + ' …' : txt)}</div>
    </div>`;
}
function convHtml(t) {
  const id = taskId(t);
  const running = isRunning(t);
  const blocks = groupEvents(t);
  const hasReply = blocks.some(b => b.kind === 'reply');
  const hasVerify = blocks.some(b => b.kind === 'verify');
  const body = blocks.map((b, i) => blockHtml(b, id + ':' + i, !!AG.openMap[id + ':' + i])).join('');

  let inner = body;
  if (!inner) {
    inner = running
      ? `<div class="jz-bubble cs-live"><span class="cs-dot cs-dot--run"></span>${esc(stateText(t))}…</div>`
      : `<div class="jz-bubble" style="color:var(--jz-muted)">（没有过程记录）</div>`;
  }
  // 历史任务(cli 的 recent)没有逐轮事件: 用最终回复/验证输出兜底
  const finalReply = (!running && !hasReply) ? replyText(t) : '';
  if (finalReply) inner += `<div class="jz-msg jz-msg--agent"><div class="jz-bubble">${esc(finalReply)}</div></div>`;
  const vout = (!running && !hasVerify) ? verifyText(t) : '';
  if (vout) inner += `<details class="cs-tool"><summary>验证输出</summary><pre class="jz-log">${esc(vout)}</pre></details>`;

  const err = !running ? statusMessage(t) : '';
  // 「接着说」的任务里每轮都有 user_msg 事件, 用户消息由 blocks 渲染;
  // 老记录(没有 user_msg)才用 taskPrompt 兜一个气泡。
  const hasUser = blocks.some(b => b.kind === 'user');
  const headBubble = hasUser ? '' : `<div class="jz-msg jz-msg--user">
      <div class="jz-bubble">${esc(taskPrompt(t))}</div>
      <div class="jz-msg__meta">${esc(taskAgent(t))} · ${shortTime(meta(t).startMs)}</div>
    </div>`;
  return `<div class="cs-task" data-task="${esc(id)}">
    ${headBubble}
    <div class="cs-conv">${inner}</div>
    ${err ? `<div class="jz-msg jz-msg--error"><div class="jz-bubble">${esc(err)}</div></div>` : ''}
    <div class="jz-msg__meta cs-task__meta">
      <span class="jz-badge ${BADGE[stateOf(t)] || ''}">${esc(stateText(t))}</span>
      <span>${taskRounds(t)} 轮 · ${taskCost(t)}</span>
      <code>${esc(id)}</code>
    </div>
  </div>`;
}
function welcomeHtml() {
  return `<div class="jz-empty cs-welcome">
    <div class="cs-welcome__logo">JZ</div>
    <div class="cs-welcome__title">给 agent 派活</div>
    <div>在下面写清楚要做的事，Enter 发送。每提交一次 = 一个任务队列，左边会多一条。</div>
    <div class="cs-welcome__tips">
      点左边某条任务看它内部的多轮对话；验证命令在「⚙ 设置」里填一次即可。
    </div>
  </div>`;
}

/* ---- 左栏: 任务队列列表(今天 / 昨天 / 一周内 / 更早) ---- */
function lastMs(t) {
  const m = meta(t);
  return m.statusMs || m.endMs || m.startMs || 0;   // 最后一次活动时间
}
function dayBucket(ms) {
  const d = new Date(ms), now = new Date();
  const day = 86400000;
  const t0 = new Date(now.getFullYear(), now.getMonth(), now.getDate()).getTime();
  const dd = new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime();
  if (dd >= t0) return 0;
  if (dd >= t0 - day) return 1;
  if (dd >= t0 - 6 * day) return 2;
  return 3;
}
const DAY_BUCKETS = ['今天', '昨天', '一周内', '更早'];

function hhmm(ms) {
  const d = new Date(ms), p = (n) => String(n).padStart(2, '0');
  return p(d.getHours()) + ':' + p(d.getMinutes());
}
/* 分组已经说明是哪一天了: 今天/昨天只给时分, 更早只给月日, 省出宽度给指令 */
function sideTime(t) {
  const ms = lastMs(t);
  const b = dayBucket(ms);
  if (b <= 1) return hhmm(ms);
  const d = new Date(ms), p = (n) => String(n).padStart(2, '0');
  return p(d.getMonth() + 1) + '-' + p(d.getDate());
}
function sideItemHtml(t) {
  const rounds = taskRounds(t);
  const prompt = taskPrompt(t) || '（空指令）';
  // 条目只占一行: 状态看圆点, 细节放 tooltip
  const tip = prompt + '\n' + stateText(t)
    + (rounds ? ' · ' + rounds + ' 轮' : '') + ' · ' + taskCost(t)
    + ' · ' + fmtTime(lastMs(t));
  const del = `<button class="ag-del" type="button" data-del="${esc(taskId(t))}"
      title="${isRunning(t) ? '取消并删除这条任务' : '删除这条记录'}" aria-label="删除">×</button>`;
  return `<div class="ag-row">
    <button class="ag-item${taskId(t) === AG.sel ? ' is-sel' : ''}" type="button"
        data-task="${esc(taskId(t))}" title="${esc(tip)}">
      <span class="cs-dot cs-dot--${dotClass(t)}"></span>
      <span class="ag-item__text">${esc(prompt)}</span>
      <span class="ag-item__time">${sideTime(t)}</span>
    </button>${del}
  </div>`;
}
function renderSide() {
  const box = document.getElementById('taskList');
  if (!box) return;
  // 每个分组里都按"最后一次会话/活动"倒序; 在跑的任务活动时间最新, 自然排在今天最上面
  const list = AG.tasks.slice().sort((a, b) => lastMs(b) - lastMs(a));
  const buckets = [[], [], [], []];
  list.forEach(t => buckets[dayBucket(lastMs(t))].push(t));
  let html = '';
  buckets.forEach((arr, i) => {
    if (!arr.length) return;
    html += `<div class="ag-group">${DAY_BUCKETS[i]} <span class="ag-group__n">${arr.length}</span></div>`
      + arr.map(sideItemHtml).join('');
  });
  box.innerHTML = html || `<div class="jz-empty">还没有任务。<br>在右边写一句，Enter 发送。</div>`;
  box.querySelectorAll('[data-task]').forEach(el => {
    el.onclick = () => selectTask(el.dataset.task, true);
  });
  box.querySelectorAll('[data-del]').forEach(el => {
    el.onclick = async (ev) => {
      ev.preventDefault(); ev.stopPropagation();
      const id = el.dataset.del;
      const t = AG.tasks.filter(x => taskId(x) === id)[0];
      const r = await deleteTaskRecords([id], { running: isRunning(t) });
      if (r.cancelled) return;
      if (!r.ok) { if (window.JzUI) JzUI.toast(r.error || '删除失败', 'fail'); return; }
      dropTasks([id]);
      if (window.JzUI) JzUI.toast(isRunning(t) ? '已取消并删除' : '已删除', 'ok');
    };
  });
  const running = AG.tasks.filter(isRunning).length;
  const sub = document.getElementById('sideSub');
  if (sub && SIDE.view === 'chats') {          // 文件/主机视图有自己的副标题
    sub.textContent = AG.tasks.length
      ? `${AG.tasks.length} 个队列${running ? ' · ' + running + ' 个在跑' : ''}`
      : '还没有任务';
  }
}
/* 点左栏某条: 右栏切成那个任务队列(它自己的多轮对话) */
function selectTask(id, scroll) {
  const t = AG.tasks.filter(x => taskId(x) === id)[0];
  AG.sel = id;
  AG.sig = null;
  adoptContext(id);      // 选中哪条, 下一条就接它的会话
  renderSide();
  renderTask(true, scroll ? (t && isRunning(t) ? 'bottom' : 'top') : '');
  if (scroll) setTab('chat');
  // 地址栏同步, 刷新/分享能回到同一个任务
  const url = new URL(location.href);
  if (id) url.searchParams.set('task', id); else url.searchParams.delete('task');
  history.replaceState(null, '', url);
  closeNav();
}
function openNav() { const el = document.getElementById('app'); if (el) el.classList.add('is-nav'); }
function closeNav() { const el = document.getElementById('app'); if (el) el.classList.remove('is-nav'); }

/* 右栏: 只渲染选中的那条任务(一条 = 一个队列), 没有选中就显示欢迎 */
function renderTask(force, scrollMode) {
  const box = document.getElementById('thread');
  if (!box) return;
  const t = AG.tasks.filter(x => taskId(x) === AG.sel)[0];
  const sig = t
    ? `${taskId(t)}:${stateOf(t)}:${taskEvents(t).length}:${replyText(t).length}:${verifyText(t).length}`
    : 'none';
  if (!force && sig === AG.sig) return;
  AG.sig = sig;

  if (!t) { box.innerHTML = welcomeHtml(); return; }
  const prevTop = box.scrollTop;
  const atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 90;
  box.innerHTML = convHtml(t);
  // 更早的事件还没加载: 顶部给一个"加载更多"(服务端一页 200 条, 起点对齐到 user_msg)
  const left = olderLeft(t);
  if (left > 0) {
    const more = document.createElement('button');
    more.className = 'thread-more';
    more.type = 'button';
    more.textContent = '⋯ 前面还有 ' + left + ' 条更早的记录，点此加载';
    more.onclick = () => loadOlderEvents(taskId(t), more);
    box.insertBefore(more, box.firstChild);
  }
  box.querySelectorAll('details[data-open]').forEach(d => {
    d.addEventListener('toggle', () => { AG.openMap[d.dataset.open] = d.open; });
  });
  if (scrollMode === 'top') box.scrollTop = 0;
  else if (scrollMode === 'bottom') box.scrollTop = box.scrollHeight;
  else box.scrollTop = atBottom ? box.scrollHeight : prevTop;
}

/* 往前翻一页事件, 拼到已加载的前面 */
async function loadOlderEvents(id, btn) {
  const t = AG.tasks.filter(x => taskId(x) === id)[0];
  if (!t) return;
  const before = olderLeft(t);
  if (!before) return;
  if (btn) { btn.disabled = true; btn.textContent = '加载中…'; }
  try {
    const d = await getJson(api('tasks/events?id=' + encodeURIComponent(id) + '&before=' + before + '&limit=200'));
    const older = d.events || [];
    const cur = AG_OLDER[id];
    AG_OLDER[id] = {
      events: older.concat(cur ? cur.events : []),
      offset: Number(d.offset) || 0
    };
    AG.sig = '';                 // 强制重画
    renderTask(true, '');
  } catch (e) {
    if (btn) { btn.disabled = false; btn.textContent = '加载失败，点此重试'; }
  }
}

function renderHead() {
  const info = AG.info || {};
  const title = document.getElementById('headTitle');
  const sub = document.getElementById('headSub');
  const acts = document.getElementById('headActs');
  const online = info.online !== false;
  title.textContent = AG.id || 'agent';
  // 工作区 = 这条任务要跑的目录(侧栏可切换); 完整路径放 title
  const proj = workdir() || info.rootDir || info.project || '';
  sub.innerHTML = `${esc(info.name || '')}${proj ? ' · 工作区 <code title="' + esc(proj) + '">' + esc(projName(proj)) + '</code>' : ''}${info.modelName ? ' · ' + esc(info.modelName) : ''}`
    + ' · ' + (online ? (AG.busy ? '<span class="jz-badge jz-badge--info">执行中</span>'
                                : '<span class="jz-badge jz-badge--ok">空闲</span>')
                      : '<span class="jz-badge">离线</span>');

  const running = AG.tasks.filter(isRunning).sort((a, b) => meta(b).startMs - meta(a).startMs)[0];
  const sel = AG.tasks.filter(t => taskId(t) === AG.sel)[0];
  acts.innerHTML = `${running ? '<button class="jz-btn jz-btn--sm jz-btn--ghost" id="btnCancelTask" type="button">取消任务</button>' : ''}
    ${sel ? '<button class="jz-btn jz-btn--sm" id="btnDelTask" type="button">删除</button>' : ''}
    ${AG.kind !== 'cli' ? `<a class="jz-btn jz-btn--sm" href="tasks?agentId=${encodeURIComponent(AG.id)}">全部任务</a>` : ''}
    <button class="jz-btn jz-btn--sm jz-btn--ghost" id="btnRefresh" type="button">刷新</button>`;
  const c = document.getElementById('btnCancelTask');
  if (c) c.onclick = async () => {
    c.disabled = true;
    const r = await cancelTask(taskId(running));
    if (window.JzUI) JzUI.toast(r.ok ? '已请求取消' : '取消失败：' + r.error, r.ok ? 'warn' : 'fail');
    loadTasks();
  };
  const d = document.getElementById('btnDelTask');
  if (d) d.onclick = async () => {
    const id = taskId(sel);
    const wasRunning = isRunning(sel);
    d.disabled = true;
    const r = await deleteTaskRecords([id], { running: wasRunning });
    if (!r.ok) {
      d.disabled = false;
      if (!r.cancelled && window.JzUI) JzUI.toast(r.error || '删除失败', 'fail');
      return;
    }
    dropTasks([id]);
    if (window.JzUI) JzUI.toast(wasRunning ? '已取消并删除' : '已删除', 'ok');
  };
  document.getElementById('btnRefresh').onclick = () => { AG.sig = ''; loadTasks(); };
}

function syncComposer() {
  const ta = document.getElementById('fPrompt');
  const send = document.getElementById('btnSend');
  const msg = document.getElementById('msg');
  const chip = document.getElementById('btnVerify');
  const running = AG.tasks.some(isRunning);
  const online = !AG.info || AG.info.online !== false;

  // 打字**永不**阻塞: 任务在跑的时候也能先把下一条写好(以前这里把输入框一起禁了,
  // 只能干等)。只有 agent 离线才禁发 —— 离线时点发送会失败, 不如直接按掉。
  ta.disabled = false;
  send.disabled = !online;
  ta.placeholder = !online ? '该 agent 已离线，只能看历史'
    : running ? '任务执行中，可以先写下一条（Enter 会排队或被告知正忙）'
    : '说一下要它做什么，Enter 发送 / Shift+Enter 换行';
  if (!online) {
    msg.dataset.blocked = '1';
    msg.textContent = '该 agent 已离线，只能查看历史';
  } else if (msg.dataset.blocked === '1') {
    msg.dataset.blocked = ''; msg.textContent = '';
  }
  const v = lsGet(VERIFY_KEY);
  if (chip) {
    chip.classList.toggle('hidden', !v);
    chip.textContent = v ? '验证: ' + v : '';
  }
}

/* ---- 会话接续: 选中哪条任务, 下一条就接着它的 contextId 聊(不额外提示) ---- */
function adoptContext(id) {
  const t = AG.tasks.filter(x => taskId(x) === (id || ''))[0];
  AG.ctx = (t && t.contextId) || '';
  // 历史任务自带工作目录: 接着它就落回它原来那个目录
  const p = (t && (meta(t) || {}).project) || '';
  if (isAbsPath(p)) lsSet(WD_KEY + AG.id, p);
}

/* ---- 工作目录: 每条任务都带着它(不改进程配置), 每个 agent 各记一份 ---- */
const WD_KEY = 'jz.workdir.';
function isAbsPath(p) { return /^([A-Za-z]:[\\/]|\/)/.test(String(p || '')); }
function workdir() {
  const saved = lsGet(WD_KEY + AG.id);
  if (saved) return saved;
  return (AG.info && (AG.info.rootDir || AG.info.project)) || '';
}
function setWorkdir(p) {
  lsSet(WD_KEY + AG.id, p || '');
  syncComposer();
  renderHead();
  if (SIDE.view === 'files') fsEnter(true);         // 文件树跟着工作区走
  else if (SIDE.view === 'hosts') loadHosts(true);  // 主机列表要更新"当前"
}

/* 目录选择器(仿 VSCode「打开文件夹」): 盘符/主目录快捷入口 + 路径输入 + 名字过滤 + 子目录列表 */
const DIRPICK_KEY = 'jz.dirpick.';
let DIR_CUR = '', DIR_PARENT = '', DIR_DIRS = [];

function dirPickStart() {
  return lsGet(DIRPICK_KEY + AG.id) || workdir() || (AG.info && AG.info.rootDir) || '';
}

async function openDirDlg() {
  const dlg = document.getElementById('dlgDir');
  if (!dlg) return;
  if (typeof dlg.showModal === 'function') { if (!dlg.open) dlg.showModal(); }
  else dlg.setAttribute('open', '');
  const f = document.getElementById('fDirFilter');
  if (f) f.value = '';
  renderDirQuick();
  await loadDir(dirPickStart());
}
function closeDirDlg() {
  const dlg = document.getElementById('dlgDir');
  if (!dlg) return;
  if (typeof dlg.close === 'function') dlg.close(); else dlg.removeAttribute('open');
}

/* 常用目录: agent 自己的根目录 + 历史任务出现过的目录 */
function renderDirQuick() {
  const box = document.getElementById('dirQuick');
  if (!box) return;
  const list = [];
  const push = (p) => { if (p && isAbsPath(p) && list.indexOf(p) < 0) list.push(p); };
  push(AG.info && AG.info.rootDir);
  AG.tasks.forEach(t => push((meta(t) || {}).project));
  push(workdir());
  box.innerHTML = list.map(p =>
    `<button class="cs-chip" type="button" data-dir="${esc(p)}" title="${esc(p)}">${esc(projName(p))}</button>`).join('');
  box.querySelectorAll('button[data-dir]').forEach(b => { b.onclick = () => loadDir(b.dataset.dir); });
}

function renderDirRoots(j) {
  const box = document.getElementById('dirRoots');
  if (!box) return;
  const chips = [];
  (j.drives || []).forEach(d => chips.push({ p: d, label: d }));
  if (j.home) chips.push({ p: j.home, label: '🏠 主目录' });
  box.innerHTML = chips.map(c =>
    `<button class="cs-chip" type="button" data-dir="${esc(c.p)}" title="${esc(c.p)}">${esc(c.label)}</button>`).join('');
  box.querySelectorAll('button[data-dir]').forEach(b => { b.onclick = () => loadDir(b.dataset.dir); });
}

/* 只画列表(过滤框变化时重画, 不重新请求) */
function renderDirList() {
  const listEl = document.getElementById('dirList');
  if (!listEl) return;
  const kw = (document.getElementById('fDirFilter') || {}).value || '';
  const q = kw.trim().toLowerCase();
  const dirs = q ? DIR_DIRS.filter(d => String(d.name).toLowerCase().indexOf(q) >= 0) : DIR_DIRS;
  listEl.innerHTML = dirs.length
    ? dirs.map(d => `<button class="dir-item" type="button" data-dir="${esc(d.path)}">
        <span class="dir-item__ico">📁</span><span class="dir-item__name">${esc(d.name)}</span></button>`).join('')
    : `<div class="jz-empty">${DIR_DIRS.length ? '没有匹配的文件夹' : '没有子文件夹'}</div>`;
  listEl.querySelectorAll('button[data-dir]').forEach(b => {
    const p = b.dataset.dir;
    if (p) b.onclick = () => { lsSet(DIRPICK_KEY + AG.id, p); loadDir(p); };
  });
}

async function loadDir(p) {
  const listEl = document.getElementById('dirList');
  const note = document.getElementById('dirNote');
  if (!listEl) return;
  listEl.innerHTML = '<div class="jz-empty">读取中…</div>';
  let r = null;
  try { r = await getJson(api('fs') + (p ? '?path=' + encodeURIComponent(p) : '')); } catch (e) { r = null; }
  const j = (r && r.json) || {};
  if (!r || !r.ok) {
    const why = typeof j === 'string' ? j : (j.error || '读不到这个目录');
    listEl.innerHTML = `<div class="jz-empty">${esc(why)}</div>`;
    return;
  }
  DIR_CUR = j.path || p || '';
  DIR_PARENT = j.parent || '';
  DIR_DIRS = j.dirs || [];
  const fPath = document.getElementById('fDirPath');
  if (fPath) fPath.value = DIR_CUR;
  const up = document.getElementById('btnDirUp');
  if (up) up.disabled = !DIR_PARENT;
  renderDirRoots(j);
  renderDirList();
  if (note) note.textContent = '当前：' + DIR_CUR;
}

function useDirDlg() {
  if (!DIR_CUR) return;
  setWorkdir(DIR_CUR);
  lsSet(DIRPICK_KEY + AG.id, DIR_CUR);
  closeDirDlg();
  if (window.JzUI) JzUI.toast('工作目录：' + DIR_CUR, 'ok');
}

function renderLog() {
  const body = document.getElementById('logBody');
  if (!body) return;
  const list = AG.tasks.slice().sort((a, b) => (meta(b).startMs || 0) - (meta(a).startMs || 0));
  body.innerHTML = list.length ? list.map(t => `<tr data-task="${esc(taskId(t))}" style="cursor:pointer">
      <td data-label="时间" class="hide-sm">${shortTime(meta(t).startMs)}</td>
      <td data-label="状态">${badge(t)}</td>
      <td data-label="指令" style="color:var(--jz-muted);font-size:var(--jz-fs-1)">${esc(taskPrompt(t).slice(0, 80))}</td>
      <td data-label="轮次" class="hide-sm">${taskRounds(t)}</td>
      <td data-label="耗时" class="hide-sm">${taskCost(t)}</td>
      <td data-label="Task" class="hide-sm"><code>${esc(taskId(t))}</code></td>
    </tr>`).join('')
    : `<tr><td colspan="6" style="color:var(--jz-muted)">还没有任务</td></tr>`;
  body.querySelectorAll('tr[data-task]').forEach(tr => {
    tr.onclick = () => selectTask(tr.dataset.task, true);
  });
}

function cardHtml(card) {
  if (!card) return '<span class="muted">没有 AgentCard</span>';
  const m = card.metadata || {};
  const skills = (card.skills || []).map(s => `<span class="skill">${esc(s.name || s.id)}
    <span class="muted">${esc((s.tags || []).join('/'))}</span></span>`).join('');
  const caps = Object.keys(card.capabilities || {}).filter(k => card.capabilities[k]).join(' · ') || '-';
  return `
    <div class="kv"><span class="k">名称</span><span class="v">${esc(card.name || '')} <span class="muted">v${esc(card.version || '?')}</span></span></div>
    <div class="kv"><span class="k">说明</span><span class="v">${esc(card.description || '-')}</span></div>
    <div class="kv"><span class="k">接口</span><span class="v"><code>${esc((card.supportedInterfaces || [{}])[0].url || '-')}</code></span></div>
    <div class="kv"><span class="k">能力</span><span class="v">${esc(caps)}</span></div>
    <div class="kv"><span class="k">skills</span><span class="v"><div class="skills">${skills || '<span class="muted">-</span>'}</div></span></div>
    <div class="kv"><span class="k">工作区</span><span class="v">${esc(m.rootDir || '-')}</span></div>
    <div class="kv"><span class="k">模型</span><span class="v">${esc(m.modelName || '-')}</span></div>`;
}
function renderNodes(card) {
  const nodes = (card && card.metadata && card.metadata.nodes) || [];
  const body = document.getElementById('nodeBody');
  if (!body) return;
  body.innerHTML = nodes.length ? nodes.map(n => `<tr>
      <td data-label="节点">${esc(n.name || '-')}</td>
      <td data-label="地址">${esc(n.target || '-')}</td>
      <td data-label="状态">${n.online ? '<span class="jz-badge jz-badge--ok">在线</span>' : '<span class="jz-badge">离线</span>'}</td>
      <td data-label="工作区" class="muted">${esc(n.rootDir || '-')}</td></tr>`).join('')
    : `<tr><td colspan="4" style="color:var(--jz-muted)">没有节点（本地执行）</td></tr>`;
}

/* ---- 终端(仅 cli 后端有; server 返回 404 就整块隐藏) ---- */
function renderTerminal(d) {
  const box = document.getElementById('termBox');
  const list = document.getElementById('termList');
  const hint = document.getElementById('termHint');
  if (!box || !list) return;
  const runs = (d && d.runs) || [];
  box.classList.toggle('hidden', !runs.length);   // 没有后台命令就整块不占版面
  hint.textContent = runs.length ? runs.length + ' 条' : '';
  list.innerHTML = runs.slice(0, 8).map(r => `<div class="ag-term__item">
      <span class="cs-dot cs-dot--${r.status === 'running' ? 'run' : (r.exit_code === 0 ? 'ok' : 'fail')}"></span>
      <code>${esc(r.run_id)}</code>
      <span class="ag-term__cmd">${esc(r.command || '')}</span>
      <span class="muted">${esc(r.status === 'running' ? '运行中' : '结束')} · ${esc(String(r.lines || 0))} 行</span>
      ${r.status === 'running' ? `<button class="jz-btn jz-btn--sm jz-btn--ghost" type="button"
        data-term-stop="${esc(r.run_id)}">停止</button>` : ''}
    </div>`).join('');
  list.querySelectorAll('[data-term-stop]').forEach(b => {
    b.onclick = async () => {
      b.disabled = true;
      await postJson(api('terminal/cancel'), { run_id: b.dataset.termStop });
      loadTerminal();
    };
  });
}
async function loadTerminal() {
  try {
    const d = await getJson(api('terminal'));
    renderTerminal(d);
  } catch (e) {
    const box = document.getElementById('termBox');
    if (box) box.classList.add('hidden');     // server 没有这个接口
  }
}

/* ---- 加载 & 订阅 ---- */
async function loadInfo() {
  let want = QS.get('id') || '';
  // 单页应用: 地址里没带 id(比如从 / 跳过来)就自动选一台在线的, 并把它写回地址栏
  if (!want) {
    try {
      const list = await getJson(api('projects'));
      const pick = (list || []).filter(a => a.online)[0] || (list || [])[0];
      if (pick) {
        want = pick.agentId;
        const u = new URL(location.href);
        u.searchParams.set('id', want);
        history.replaceState(null, '', u);
      }
    } catch (e) { /* 拿不到就按原来走, 下面的请求会报错 */ }
  }
  const d = await getJson(api('agent' + (want ? '?id=' + encodeURIComponent(want) : '')));
  AG.info = d.info || {};
  AG.card = d.card || null;
  AG.kind = d.kind || '';
  AG.id = AG.info.agentId || want;
  // 从「主机」视图点别的 agent 的目录跳过来: 先落到本机记录, 再把 dir 从地址栏抹掉
  const u0 = new URL(location.href);
  const dq = u0.searchParams.get('dir');
  if (dq && isAbsPath(dq)) {
    lsSet(WD_KEY + AG.id, dq);
    u0.searchParams.delete('dir');
    history.replaceState(null, '', u0);
  }

  document.getElementById('cardBody').innerHTML = cardHtml(AG.card);
  renderNodes(AG.card);
}

async function loadTasks() {
  const q = ['limit=50', 'origin=user'];
  if (AG.id) q.push('agentId=' + encodeURIComponent(AG.id));
  try {
    const data = await getJson(api('tasks?' + q.join('&')));
    AG.tasks = data.tasks || [];
  } catch (e) {
    document.getElementById('msg').textContent = '读取任务失败: ' + e;
    return;
  }
  AG.busy = AG.tasks.some(isRunning);
  if (AG.sel && !AG.tasks.some(t => taskId(t) === AG.sel)) AG.sel = '';

  let scrollMode = '';
  const want = QS.get('task');       // 深链: ?id=..&task=..
  if (!AG.sel && want && AG.tasks.some(t => taskId(t) === want)) {
    AG.sel = want;
    AG.sig = null;
    scrollMode = 'top';
  }
  if (!AG.sel && AG.tasks.length) {
    // 默认选中: 有在跑的选它, 否则选最新的那条
    const newest = (a, b) => (meta(b).startMs || 0) - (meta(a).startMs || 0);
    const run = AG.tasks.filter(isRunning).sort(newest)[0];
    const pick = run || AG.tasks.slice().sort(newest)[0];
    AG.sel = taskId(pick);
    AG.sig = null;
    adoptContext(AG.sel);
    scrollMode = isRunning(pick) ? 'bottom' : 'top';
  }

  renderHead(); renderTask(false, scrollMode); renderLog(); renderSide(); syncComposer();
  watchRunning();
  schedulePoll();
}

async function loadDetail(id) {
  try {
    const t = await getJson(api('tasks/detail?id=' + encodeURIComponent(id)));
    const i = AG.tasks.findIndex(x => taskId(x) === id);
    if (i >= 0) AG.tasks[i] = t; else AG.tasks.push(t);
    AG.busy = AG.tasks.some(isRunning);
    renderHead(); renderSide(); syncComposer();
    if (id === AG.sel) renderTask(false, isRunning(t) ? 'bottom' : '');
    if (isTerminal(t)) { unwatch(); loadTasks(); loadTerminal(); }
  } catch (e) { /* 单条读失败不影响整页 */ }
}

/* ---- 常连(下行) ----
   以前是"每条任务开一条 SSE, 终态服务端关流" → 每跑完一个任务浏览器就重连一次,
   看着就是连接中断。现在整页只开一条 /api/events: 所有任务事件都在上面(taskId 自己筛),
   服务端不因为任务结束而关它; 断了 EventSource 自动重连, 重连后先收一份 hello 快照。
   上行(提交/取消/删除)仍是普通 POST —— SSE 是单向的。 */
let EVT = null;
let TASKS_REFRESH = 0;
function scheduleTasksRefresh() {
  if (TASKS_REFRESH) return;
  TASKS_REFRESH = setTimeout(() => { TASKS_REFRESH = 0; loadTasks(); }, 800);
}
function onAgentsFrame(m) {
  if (!m || !m.agents) return;
  const me = (m.agents || []).filter(a => a.agentId === AG.id)[0];
  if (!me) return;
  AG.info = Object.assign({}, AG.info, {
    online: me.online !== false, busy: !!me.busy,
    round: me.round || 0, taskId: me.taskId || ''
  });
  AG.busy = !!me.busy;
  renderHead(); syncComposer();
}
/* 下行是判断"连接是否还活着"的主依据: 服务端每 5s 推一帧 agents,
   超过 20s 一帧都没收到 = 下行真断了(本地环境没有网络抖动, 等价于 server 挂了)。 */
let EVT_LAST = 0, EVT_CHK = 0;
function evtBeat() { EVT_LAST = Date.now(); setConn(true); }
function startEvtWatch() {
  clearInterval(EVT_CHK);
  EVT_CHK = setInterval(() => {
    if (!EVT_LAST) return;
    if (Date.now() - EVT_LAST > 20000) setConn(false, '下行 20s 无数据');
  }, 5000);
}

function openEvents() {
  if (typeof EventSource === 'undefined' || EVT) return;
  EVT = new EventSource(withToken(api('events')));
  EVT.addEventListener('hello', (e) => {
    evtBeat();
    try { onAgentsFrame(JSON.parse(e.data)); } catch (_) { /* 忽略 */ }
  });
  EVT.addEventListener('agents', (e) => {
    evtBeat();
    try { onAgentsFrame(JSON.parse(e.data)); } catch (_) { /* 忽略 */ }
  });
  EVT.addEventListener('task', (e) => {
    evtBeat();                           // 收到帧 = 链路活着
    let m = null;
    try { m = JSON.parse(e.data); } catch (_) { m = null; }
    if (!m || !m.taskId) return;
    if (m.taskId === AG.sel) loadDetail(m.taskId);
    scheduleTasksRefresh();              // 侧栏/状态要跟着变(限流 800ms)
  });
  EVT.addEventListener('deleted', (e) => {
    evtBeat();
    let m = null;
    try { m = JSON.parse(e.data); } catch (_) { m = null; }
    if (!m || !m.taskId) return;
    dropTasks([m.taskId]);
    if (m.taskId === AG.sel) { AG.sel = ''; renderTask(true); }
    if (window.JzUI) JzUI.toast('这条任务已被删除', 'warn');
  });
  // 断开时 EventSource 自己会重连(readyState=CONNECTING), 这里不急着判死:
  // 真断了会体现为"20s 收不到任何帧"(见 startEvtWatch)
  EVT.onerror = () => { /* 交给 EVT_LAST 判断 */ };
  startEvtWatch();
}
function unwatch() { /* 已改为常连, 保留空实现给老调用点 */ }
function watchRunning() { /* 同上 */ }

/* 没有在跑的任务时也要能发现断线: 每 15s 探一次 /api/status(可见时才探) */
let CONN_PROBE = 0;
function startConnProbe() {
  clearInterval(CONN_PROBE);
  CONN_PROBE = setInterval(() => {
    if (document.hidden) return;
    getJson(api('status')).catch(() => { /* setConn 已在里面处理 */ });
  }, 15000);
}
function schedulePoll() {
  clearTimeout(AG.poll);
  AG.poll = 0;
  if (document.hidden || !AG.tasks.some(isRunning)) return;
  AG.poll = setTimeout(() => { loadTasks(); }, 5000);
}

let SUBMIT_LOCK = false;
async function doSend() {
  const ta = document.getElementById('fPrompt');
  const msg = document.getElementById('msg');
  const text = ta.value.trim();
  if (!text) { msg.textContent = '先写点要它做的事'; ta.focus(); return; }
  if (!AG.id) { msg.textContent = '还没有 agent'; return; }
  if (SUBMIT_LOCK) return;
  SUBMIT_LOCK = true;
  ta.disabled = true; document.getElementById('btnSend').disabled = true;
  msg.dataset.blocked = ''; msg.textContent = '已提交，等待 agent 接单…';

  const ctxKey = 'jz.ctx.' + AG.id;
  // ⚠ 一定要 try/finally: 以前网络一抖(fetch 抛错), SUBMIT_LOCK 和"发送"按钮就永远回不来
  let r = null, fail = '';
  try {
    r = await sendMessage({
      agentId: AG.id,
      prompt: text,
      verifyCmd: lsGet(VERIFY_KEY),
      // 选中了哪条任务, 这一轮就追加到那条任务里(同一个 taskId, 聊天区连续);
      // 没选(=新建)就照旧开一条新任务, 沿用本机记着的上次会话。
      taskId: AG.sel || '',
      // 接着说时 contextId 由后端按原任务取, 不用前端传; 新建才用本机记着的上次会话
      contextId: AG.sel ? '' : lsGet(ctxKey),
      // 工作目录: 这条任务在哪个目录跑(网页上切换过就带它)
      rootDir: workdir()
    });
    if (!r.ok) fail = r.error || '下发失败';
  } catch (e) {
    if (window.console) console.warn('提交失败', e);
    if (e && e.name === 'AbortError')
      fail = '请求超时(15s)，检查网络或该 agent 是否在线';
    else if (typeof navigator !== 'undefined' && navigator.onLine === false)
      fail = '本机已离线';
    else
      fail = '连不上服务（server / agent 可能已停，或地址不通）';
  } finally {
    SUBMIT_LOCK = false;
    ta.disabled = false; document.getElementById('btnSend').disabled = false;
  }
  if (!r || !r.ok) {
    syncComposer();
    msg.textContent = '下发失败：' + fail;
    if (window.JzUI) JzUI.toast('下发失败', 'fail');
    return;
  }

  if (r.contextId) lsSet(ctxKey, r.contextId);   // 同一 agent 的后续任务接着这条会话
  AG.ctx = r.contextId || '';                    // 后面再发就落在这条会话上
  ta.value = ''; ta.style.height = '';
  msg.textContent = '';
  AG.sel = r.id;                                 // 新任务: 左栏选中 + 右栏滚到它
  await loadTasks();
  selectTask(r.id, true);
  ta.focus();
}

/* ---- 设置弹窗 ---- */
function openSettings() {
  const dlg = document.getElementById('dlgSettings');
  document.getElementById('fVerify').value = lsGet(VERIFY_KEY);
  document.getElementById('fToken').value = lsGet(TOKEN_KEY);
  if (typeof dlg.showModal === 'function') { if (!dlg.open) dlg.showModal(); }
  else dlg.setAttribute('open', '');
}
function closeSettings() {
  const dlg = document.getElementById('dlgSettings');
  if (typeof dlg.close === 'function') dlg.close(); else dlg.removeAttribute('open');
}
function saveSettings() {
  lsSet(VERIFY_KEY, document.getElementById('fVerify').value.trim());
  lsSet(TOKEN_KEY, document.getElementById('fToken').value.trim());
  closeSettings();
  syncComposer();
  if (window.JzUI) JzUI.toast('设置已保存', 'ok');
}

/* ---- tab ---- */
const TABS = ['chat', 'log', 'card'];
function curTab() { const t = (location.hash || '#chat').slice(1); return TABS.indexOf(t) >= 0 ? t : 'chat'; }
function setTab(name) {
  document.querySelectorAll('#tabs .tab').forEach(a => a.classList.toggle('is-on', a.dataset.tab === name));
  document.querySelectorAll('.pane').forEach(p => p.classList.toggle('hidden', p.dataset.pane !== name));
}

/* 绑定 onclick 的 null 安全版本: 少一个元素不该把整页初始化打断
   (之前 btnVerify 被误删, 直接抛 TypeError, 后面所有绑定都没跑) */
function on(id, fn) {
  const el = document.getElementById(id);
  if (el) el.onclick = fn;
  return el;
}

function initChatPage() {
  on('btnSend', doSend);
  on('btnSettings', openSettings);
  on('btnVerify', openSettings);
  on('btnSetSave', saveSettings);
  on('btnSetClose', closeSettings);
  const dlgSet = document.getElementById('dlgSettings');
  if (dlgSet) dlgSet.addEventListener('click', (e) => {
    if (e.target.id === 'dlgSettings') closeSettings();
  });

  // 工作目录选择(弹窗由「主机」里的「＋ 打开文件夹…」打开)
  on('btnDirClose', closeDirDlg);
  on('btnDirUse', useDirDlg);
  on('btnDirGo', () => {
    const el = document.getElementById('fDirPath');
    const v = el ? el.value.trim() : '';
    if (v) loadDir(v);
  });
  on('btnDirUp', () => { if (DIR_PARENT) loadDir(DIR_PARENT); });
  on('btnDirCancel', closeDirDlg);
  const fDirPath = document.getElementById('fDirPath');
  if (fDirPath) fDirPath.addEventListener('keydown', (e) => {
    if (e.key !== 'Enter' || e.isComposing) return;
    e.preventDefault();
    const v = e.target.value.trim();
    if (v) loadDir(v);
  });
  const fDirFilter = document.getElementById('fDirFilter');
  if (fDirFilter) {
    fDirFilter.addEventListener('input', renderDirList);
    fDirFilter.addEventListener('keydown', (e) => {
      if (e.key !== 'Enter' || e.isComposing) return;
      e.preventDefault();
      // 回车: 有唯一匹配就进去, 否则用第一个
      const first = document.querySelector('#dirList button[data-dir]');
      if (first) { const p = first.dataset.dir; lsSet(DIRPICK_KEY + AG.id, p); loadDir(p); }
    });
  }
  const dlgDir = document.getElementById('dlgDir');
  if (dlgDir) dlgDir.addEventListener('click', (e) => {
    if (e.target.id === 'dlgDir') closeDirDlg();
  });

  // Activity Bar: 文件 / 主机 / 会话
  document.querySelectorAll('#rail .rail-btn[data-view]').forEach(b => {
    b.onclick = () => setSideView(b.dataset.view);
  });
  initSplitter();      // 侧栏宽度可拖
  // rail 底部的 ⚙ 先只占位: 功能待定(设置目前仍从输入区上方的「⚙ 设置」进)
  on('railSettings', null);
  on('railTasks', () => { location.href = 'tasks'; });
  on('btnFsRefresh', () => fsEnter(true));
  on('btnSideCollapse', () => { FS.collapsed = !FS.collapsed; fsApplyCollapse(); });
  on('btnFsCollapse', () => { FS.open = {}; fsRender(); });
  on('btnPrevBack', () => setTab('chat'));
  on('btnPrevCopy', () => {
    if (!PREV_TEXT || !navigator.clipboard) return;
    navigator.clipboard.writeText(PREV_TEXT)
      .then(() => { if (window.JzUI) JzUI.toast('已复制', 'ok'); })
      .catch(() => {});
  });

  const ta = document.getElementById('fPrompt');
  ta.addEventListener('keydown', (e) => {
    if (e.key !== 'Enter' || e.shiftKey || e.isComposing || e.keyCode === 229) return;
    e.preventDefault();
    if (!document.getElementById('btnSend').disabled) doSend();
  });
  ta.addEventListener('input', () => {
    ta.style.height = 'auto';
    ta.style.height = Math.min(ta.scrollHeight, Math.round(window.innerHeight * 0.3)) + 'px';
  });

  // 左栏(窄屏是抽屉)
  const btnNew = document.getElementById('btnNewTask');
  if (btnNew) btnNew.onclick = () => {
    setTab('chat');
    AG.ctx = '';                       // 新建 = 另起会话, 不接上一条
    lsSet('jz.ctx.' + AG.id, '');
    selectTask('', false);
    const box = document.getElementById('thread');
    if (box) box.scrollTop = box.scrollHeight;
    const t = document.getElementById('fPrompt');
    if (t) t.focus();
    closeNav();
  };
  const btnSide = document.getElementById('btnSide');
  if (btnSide) btnSide.onclick = openNav;
  const btnSideClose = document.getElementById('btnSideClose');
  if (btnSideClose) btnSideClose.onclick = closeNav;
  const scrim = document.getElementById('scrim');
  if (scrim) scrim.onclick = closeNav;

  setTab(curTab());
  window.addEventListener('hashchange', () => setTab(curTab()));
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) { clearTimeout(AG.poll); AG.poll = 0; }
    else { loadTasks(); }
  });

  // 一条常连(下行): 任务事件 + agent 快照 + 删除通知, 整页只开一次。
  openEvents();
  // 兜底心跳: 页面隐藏时常连被浏览器节流, 可见时 15s 探一次
  startConnProbe();
  window.addEventListener('offline', () => setConn(false, '浏览器已离线'));
  window.addEventListener('online', () => { setConn(true); loadTasks(); });

  const termBox = document.getElementById('termBox');
  if (termBox) termBox.addEventListener('toggle', () => { if (termBox.open) loadTerminal(); });

  // 默认进「会话」(打开页面通常是接着聊); 上次选过哪个就还是哪个
  setSideView(lsGet(SIDE_KEY) || 'chats');

  loadInfo()
    .then(() => {
      document.title = AG.id + ' · JZAgent';
      if (SIDE.view === 'files') fsEnter(true);      // 工作区依赖 AG.info, 这时候才能读
      return loadTasks();
    })
    .then(() => { syncComposer(); loadTerminal(); })
    .catch((e) => {
      document.getElementById('headSub').textContent = '读取失败: ' + e;
      document.getElementById('thread').innerHTML =
        `<div class="jz-empty">拿不到这台 agent 的信息：${esc(String(e))}<br>
          server 上不存在这个 id，或者它已经被移除。</div>`;
    });
}

/* ============================ 侧栏视图(仿 VSCode Activity Bar) ============================ */
/* 最左侧一列按钮切三个视图: 文件 / 主机 / 会话。同一时间只显示一个 side-view。 */

const SIDE_KEY = 'jz.sideview';
const SIDE_TITLE = { files: '文件', hosts: '主机', chats: '会话' };
const SIDE = { view: 'files' };

function setSideView(name) {
  if (!SIDE_TITLE[name]) name = 'files';
  SIDE.view = name;
  lsSet(SIDE_KEY, name);
  document.querySelectorAll('#rail .rail-btn[data-view]').forEach(b =>
    b.classList.toggle('is-on', b.dataset.view === name));
  document.querySelectorAll('.side-view').forEach(v =>
    v.classList.toggle('hidden', v.dataset.view !== name));
  // 文件视图的标题栏另外一套: 折叠箭头 + 大写项目名 + 右侧图标组(刷新/折叠全部…)
  const isFiles = name === 'files';
  const t = document.getElementById('sideTitle');
  const sub = document.getElementById('sideSub');
  const toggle = document.getElementById('btnSideCollapse');
  const acts = document.getElementById('sideActs');
  if (t) {
    t.classList.toggle('is-file', isFiles);
    t.textContent = isFiles ? fsTitle() : SIDE_TITLE[name];
  }
  if (sub) sub.classList.toggle('hidden', isFiles);
  if (toggle) toggle.classList.toggle('is-on', isFiles);
  if (acts) acts.classList.toggle('is-on', isFiles);

  if (isFiles) fsEnter();
  else if (name === 'hosts') {
    if (HOSTS.loaded) renderHosts();     // 有缓存: 先画, 再后台刷新(不闪"读取中")
    loadHosts(true);
  }
  else renderSide();
}

/* ---- 侧栏宽度可拖(rail | 侧栏 | 拖拽条 | 主区) ---- */
const SIDEW_KEY = 'jz.sidew';

function applySideWidth(px) {
  const app = document.getElementById('app');
  if (app && px > 0) app.style.setProperty('--ag-side-w', Math.round(px) + 'px');
}

function initSplitter() {
  const app = document.getElementById('app');
  const sp = document.getElementById('agSplitter');
  const side = document.getElementById('agSide');
  if (!app || !sp || !side) return;

  applySideWidth(parseInt(lsGet(SIDEW_KEY), 10) || 0);   // 上次拖到的宽度

  let dragging = false, startX = 0, startW = 0, pid = null;
  const clamp = (w) => Math.max(200, Math.min(560, Math.min(w, Math.floor(window.innerWidth * 0.6))));

  sp.addEventListener('pointerdown', (e) => {
    if (e.button != null && e.button !== 0) return;
    dragging = true; pid = e.pointerId;
    startX = e.clientX;
    startW = side.getBoundingClientRect().width;
    sp.classList.add('is-drag');
    document.body.classList.add('is-col-resize');
    try { sp.setPointerCapture(e.pointerId); } catch (err) {}
    e.preventDefault();
  });
  sp.addEventListener('pointermove', (e) => {
    if (!dragging) return;
    applySideWidth(clamp(startW + (e.clientX - startX)));
  });
  const endDrag = () => {
    if (!dragging) return;
    dragging = false;
    sp.classList.remove('is-drag');
    document.body.classList.remove('is-col-resize');
    lsSet(SIDEW_KEY, String(Math.round(side.getBoundingClientRect().width)));
    if (pid != null) {
      try { sp.releasePointerCapture(pid); } catch (err) {}
      pid = null;
    }
  };
  sp.addEventListener('pointerup', endDrag);
  sp.addEventListener('pointercancel', endDrag);
  sp.addEventListener('dblclick', () => {          // 双击 = 恢复默认宽度
    lsSet(SIDEW_KEY, '');
    app.style.removeProperty('--ag-side-w');
  });
}

/* ---- 文件: 当前工作区的只读文件树(懒加载子目录) ---- */
const FS = { root: '', open: {}, nodes: {}, collapsed: false };

/* 标题栏显示的项目名: 目录末段, 大写(仿 VSCode) */
function fsTitle() {
  const p = workdir();
  return p ? String(projName(p) || p).toUpperCase() : '工作区';
}

/* 标题栏左边的 ⌄: 折叠整个文件树(标题栏保留) */
function fsApplyCollapse() {
  const tree = document.getElementById('fsTree');
  const btn = document.getElementById('btnSideCollapse');
  if (tree) tree.classList.toggle('hidden', !!FS.collapsed);
  if (btn) btn.classList.toggle('is-collapsed', !!FS.collapsed);
}

function fsEnter(force) {
  const root = workdir();
  if (force || FS.root !== root) { FS.root = root; FS.open = {}; FS.nodes = {}; }
  const title = document.getElementById('sideTitle');
  if (title && SIDE.view === 'files') title.textContent = fsTitle();
  fsApplyCollapse();
  if (!root) {
    const box = document.getElementById('fsTree');
    if (box) box.innerHTML = '<div class="jz-empty">还没有工作区：在「主机」里选一个目录</div>';
    return;
  }
  if (!FS.nodes[root]) { fsRender(); fsLoad(root).then(fsRender); return; }
  fsRender();
}

async function fsLoad(path) {
  if (FS.nodes[path]) return FS.nodes[path];
  const node = { dirs: [], files: [], error: '' };
  try {
    const j = await getJson(api('fs') + '?path=' + encodeURIComponent(path));
    node.dirs = j.dirs || [];
    node.files = j.files || [];
  } catch (e) {
    node.error = '读不到：' + (e && e.message ? e.message : e);
  }
  FS.nodes[path] = node;
  return node;
}

function fmtSize(n) {
  n = Number(n) || 0;
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(n < 10240 ? 1 : 0) + ' KB';
  return (n / 1024 / 1024).toFixed(1) + ' MB';
}

function fsRender() {
  const box = document.getElementById('fsTree');
  if (!box) return;
  if (!FS.root) { box.innerHTML = '<div class="jz-empty">还没有工作区</div>'; return; }
  box.innerHTML = fsRows(FS.root, 0);
  box.querySelectorAll('button[data-dir]').forEach(b => {
    b.onclick = async () => {
      const path = b.dataset.dir;
      if (FS.open[path]) { delete FS.open[path]; fsRender(); return; }
      FS.open[path] = true;
      fsRender();
      if (!FS.nodes[path]) { await fsLoad(path); fsRender(); }
    };
  });
  box.querySelectorAll('button[data-file]').forEach(b => {
    b.onclick = () => openPreview(b.dataset.file);
  });
}

function fsRows(path, depth) {
  const n = FS.nodes[path];
  const ind = 6 + depth * 12;
  if (!n) return `<div class="fs-empty" style="padding-left:${ind}px">读取中…</div>`;
  if (n.error) return `<div class="fs-empty" style="padding-left:${ind}px">${esc(n.error)}</div>`;
  if (!n.dirs.length && !n.files.length)
    return `<div class="fs-empty" style="padding-left:${ind}px">（空）</div>`;
  let out = '';
  n.dirs.forEach(d => {
    const open = !!FS.open[d.path];
    out += `<button class="fs-row" type="button" data-dir="${esc(d.path)}" style="padding-left:${ind}px">
      <span class="fs-row__ico">${open ? '▾' : '▸'}</span>
      <span class="fs-row__name">📁 ${esc(d.name)}</span></button>`;
    if (open) out += fsRows(d.path, depth + 1);
  });
  n.files.forEach(f => {
    out += `<button class="fs-row" type="button" data-file="${esc(f.path)}" style="padding-left:${ind + 14}px">
      <span class="fs-row__ico">📄</span>
      <span class="fs-row__name">${esc(f.name)}</span>
      <span class="fs-row__meta">${fmtSize(f.size)}</span></button>`;
  });
  return out;
}

/* ---- 文件预览(只读): 后端给原始字节, 这里判编码 ---- */
let PREV_TEXT = '';

function decodeBytes(bytes) {
  if (!bytes.length) return { text: '', encoding: 'utf-8' };
  for (let i = 0; i < bytes.length && i < 8192; i++)
    if (bytes[i] === 0) return { binary: true };          // 含 \0 当二进制
  try { return { text: new TextDecoder('utf-8', { fatal: true }).decode(bytes), encoding: 'utf-8' }; }
  catch (e) { /* 不是合法 UTF-8 */ }
  try { return { text: new TextDecoder('gb18030').decode(bytes), encoding: 'gb18030' }; }
  catch (e) { return { binary: true }; }
}

async function openPreview(path) {
  PREV_TEXT = '';
  const body = document.getElementById('prevBody');
  const meta = document.getElementById('prevMeta');
  const copy = document.getElementById('btnPrevCopy');
  const pp = document.getElementById('prevPath');
  if (pp) pp.textContent = path;
  if (meta) meta.textContent = '读取中…';
  if (body) body.textContent = '';
  if (copy) copy.disabled = true;
  setTab('preview');

  let r = null;
  try { r = await fetchT(withToken(api('fs/raw') + '?path=' + encodeURIComponent(path)), null, 15000); }
  catch (e) { if (meta) meta.textContent = '读取失败：' + netReason(e); return; }
  if (!r || !r.ok) {
    let msg = 'HTTP ' + (r ? r.status : '?');
    try { msg = await r.text(); } catch (e) {}
    if (meta) meta.textContent = msg;
    return;
  }

  const size = Number(r.headers.get('X-File-Size') || 0);
  const bytes = new Uint8Array(await r.arrayBuffer());
  const dec = decodeBytes(bytes);
  if (dec.binary) {
    if (meta) meta.textContent = '二进制文件 · ' + fmtSize(size);
    return;
  }
  PREV_TEXT = dec.text;
  if (body) body.textContent = dec.text;
  if (copy) copy.disabled = false;
  const truncated = size > bytes.length;
  if (meta) meta.textContent = `${dec.encoding.toUpperCase()} · ${fmtSize(size)}`
    + (truncated ? ' · 已截断（只显示前 1MB）' : '');
}

/* ---- 主机: 所有 agent + 各自用过的目录(不显示 node) ---- */
/* 一次请求 /api/projects 就够(后端从任务记录聚合, 不再拉 tasks 全量);
   切视图时先用缓存画, 再后台静默刷新 —— 不会每次都闪"读取中"。 */
const HOSTS = { list: [], loaded: false, busy: false, collapsed: {}, sel: '' };

async function loadHosts(force) {
  const box = document.getElementById('hostsList');
  if (!box) return;
  if (HOSTS.loaded && !force) { renderHosts(); return; }   // 有缓存: 立刻画
  if (HOSTS.busy) return;
  HOSTS.busy = true;
  if (!HOSTS.loaded) box.innerHTML = '<div class="jz-empty">读取中…</div>';

  let arr = null;
  try { arr = await getJson(api('projects')); } catch (e) { arr = null; }
  HOSTS.busy = false;
  if (!arr) {
    if (!HOSTS.loaded) box.innerHTML = '<div class="jz-empty">读不到主机列表</div>';
    return;
  }

  HOSTS.list = (arr || []).map(a => {
    const cur = AG.id && a.agentId === AG.id ? workdir() : '';
    const list = (a.projects || [])
      .map(p => ({ path: p.path, lastMs: p.lastMs || 0, count: p.count || 0 }))
      .filter(p => isAbsPath(p.path));
    // 当前工作区如果还没出现在任务记录里(刚打开的新文件夹), 也列进来排最前
    if (cur && !list.some(p => p.path === cur))
      list.unshift({ path: cur, lastMs: Date.now(), count: 0 });
    else if (cur) list.sort((x, y) => (x.path === cur ? -1 : y.path === cur ? 1 : y.lastMs - x.lastMs));
    else list.sort((x, y) => y.lastMs - x.lastMs);
    const root = a.rootDir || '';
    if (root && isAbsPath(root) && !list.some(p => p.path === root))
      list.push({ path: root, lastMs: 0, count: 0 });
    return { agentId: a.agentId, online: a.online !== false, rootDir: root, dirs: list };
  }).sort((x, y) => (x.agentId === AG.id ? -1 : y.agentId === AG.id ? 1 : x.agentId.localeCompare(y.agentId)));

  HOSTS.loaded = true;
  renderHosts();
}

function renderHosts() {
  const box = document.getElementById('hostsList');
  if (!box) return;
  if (!HOSTS.list.length) { box.innerHTML = '<div class="jz-empty">没有可用的 agent</div>'; return; }
  const cur = workdir();
  box.innerHTML = HOSTS.list.map(h => {
    const open = !HOSTS.collapsed[h.agentId];
    return `
    <div class="host">
      <button class="host-row" type="button" data-agent="${esc(h.agentId)}">
        <span class="host-row__arrow">${open ? '▾' : '▸'}</span>
        <span class="host-row__dot${h.online ? '' : ' off'}"></span>
        <span class="host-row__name">${esc(h.agentId)}</span>
        ${h.agentId === AG.id ? '<span class="host-row__cur">当前</span>' : ''}
      </button>
      ${open ? h.dirs.map(d => {
        const key = h.agentId + '|' + d.path;
        const cls = (d.path === cur && h.agentId === AG.id) ? ' is-on'
                  : (HOSTS.sel === key ? ' is-sel' : '');
        return `<button class="dir-row${cls}" type="button"
          data-agent="${esc(h.agentId)}" data-dir="${esc(d.path)}" title="${esc(d.path)}">
          <span class="dir-row__name">${esc(projName(d.path))}</span>
          <span class="dir-row__path">${esc(d.path)}</span>
          <span class="dir-row__go" title="在 ${esc(h.agentId)} 上打开">→</span>
        </button>`;
      }).join('') : ''}
      ${open && h.agentId === AG.id ? `<button class="dir-row dir-row--add" type="button" data-add="1">
          <span class="dir-row__name">＋ 打开文件夹…</span></button>` : ''}
    </div>`;
  }).join('');

  // 点主机行 = 展开/折叠(VSCode 的行为); 换 agent 点它下面的目录
  box.querySelectorAll('.host-row').forEach(b => {
    b.onclick = () => {
      const id = b.dataset.agent;
      if (HOSTS.collapsed[id]) delete HOSTS.collapsed[id];
      else HOSTS.collapsed[id] = true;
      renderHosts();
    };
  });
  // 点行只选中; 只有点行尾的 → 才真的打开(当前 agent = 切工作区+去文件树, 别的 = 跳过去)
  box.querySelectorAll('.dir-row[data-dir]').forEach(b => {
    b.onclick = (ev) => {
      const a = b.dataset.agent, d = b.dataset.dir;
      if (!ev.target.closest('.dir-row__go')) {
        HOSTS.sel = a + '|' + d;
        renderHosts();
        return;
      }
      if (a === AG.id) { if (d !== workdir()) setWorkdir(d); setSideView('files'); }
      else location.href = '?id=' + encodeURIComponent(a) + '&dir=' + encodeURIComponent(d);
    };
  });
  box.querySelectorAll('.dir-row--add').forEach(b => { b.onclick = openDirDlg; });
  const sub = document.getElementById('sideSub');
  if (sub && SIDE.view === 'hosts') sub.textContent = HOSTS.list.length + ' 个 agent';
}

/* ============================ /tasks  Task 记录 ============================ */

let ES = null, DETAIL_ID = '', CURRENT_TASK = null;

function closeDetailWatch() { if (ES) { ES.close(); ES = null; } }
function openStream(id) {
  if (typeof EventSource === 'undefined') return;
  const es = new EventSource(withToken(api('tasks/subscribe?id=' + encodeURIComponent(id))));
  ES = es;
  const onAny = () => { if (DETAIL_ID === id) loadDetail(id, true).catch(() => {}); };
  ['task', 'status_update', 'artifact_update'].forEach(n => es.addEventListener(n, onAny));
}
function taskRows(list) {
  if (!list.length) return `<tr><td colspan="8" style="color:var(--jz-muted)">没有符合条件的 Task</td></tr>`;
  return list.map(t => `<tr data-task="${esc(taskId(t))}" style="cursor:pointer">
      <td data-label="Task">${esc(taskId(t))}</td>
      <td data-label="agent">${esc(taskAgent(t))}</td>
      <td data-label="状态">${badge(t)}${taskOrigin(t) !== 'user' ? ' <span class="jz-badge">agent 内部</span>' : ''}</td>
      <td data-label="最近事件" class="hide-sm">${esc(meta(t).lastEventType || '-')}</td>
      <td data-label="耗时" class="hide-sm">${taskCost(t)}</td>
      <td data-label="提交时间" class="hide-sm">${fmtTime(meta(t).startMs)}</td>
      <td data-label="指令" style="color:var(--jz-muted);font-size:var(--jz-fs-1)">${esc(taskPrompt(t).slice(0, 60))}</td>
      <td data-label="操作"><button class="jz-btn jz-btn--sm jz-btn--ghost" type="button" data-del="${esc(taskId(t))}"
        title="${isRunning(t) ? '取消并删除' : '删除记录'}">删除</button></td>
    </tr>`).join('');
}
function eventHtml(events) {
  if (!events || !events.length) return '<div class="timeline__empty">还没有过程事件</div>';
  return events.map(e => {
    let d = e.data == null ? '' : (typeof e.data === 'string' ? e.data : JSON.stringify(e.data));
    if (d.length > 240) d = d.slice(0, 240) + '…';
    return `<div class="timeline__item"><span class="timeline__seq">${esc(e.seq || '')}</span>
      <span class="timeline__type">${esc(e.type || 'event')}</span>
      <span class="timeline__at">${fmtTime(e.at)}</span>
      <div class="timeline__data">${esc(d)}</div></div>`;
  }).join('');
}
function artifactHtml(arts) {
  if (!arts || !arts.length) return '<div class="timeline__empty">还没有产物</div>';
  return arts.map(a => `<div class="artifact"><div class="artifact__head">${esc(a.name || a.artifactId || 'artifact')}
      <span class="muted">${esc(a.artifactId || '')}</span></div>
      <pre class="jz-log">${esc(partText(a.parts))}</pre></div>`).join('');
}
function historyHtml(hist) {
  if (!hist || !hist.length) return '';
  return hist.map(m => `<div class="msg ${m.role === 'ROLE_USER' ? 'msg--user' : 'msg--agent'}">
      <div class="bubble">${esc(partText(m.parts))}</div>
      <div class="msg__meta">${esc(m.role === 'ROLE_USER' ? '下发指令' : 'agent')}</div>
    </div>`).join('');
}
function renderDetail(t) {
  CURRENT_TASK = t;
  const detailBox = document.getElementById('detail');
  if (!detailBox) return;                       // 页面模板里没有详情面板就什么也不做
  detailBox.classList.remove('hidden');
  document.getElementById('detailHead').innerHTML = `<div class="task-head">
      <div><span class="task-head__id">${esc(taskId(t))}</span> ${badge(t)}
        ${taskOrigin(t) !== 'user' ? '<span class="jz-badge">agent 内部</span>' : ''}</div>
      <div class="muted">${esc(taskAgent(t))} · ${taskCost(t)} · ${taskRounds(t)} 轮 · ${fmtTime(meta(t).startMs)} → ${fmtTime(meta(t).endMs)}</div>
      <div class="muted">contextId: <code>${esc(t.contextId || '-')}</code>
        ${meta(t).parentTaskId ? ' · 父任务 <code>' + esc(meta(t).parentTaskId) + '</code>' : ''}</div>
    </div>`;
  const acts = document.getElementById('detailActs');
  acts.innerHTML = `
    ${isRunning(t) ? `<button class="jz-btn jz-btn--sm" id="btnCancelTask">取消 Task</button>` : ''}
    <button class="jz-btn jz-btn--sm" id="btnDeleteTask">删除记录</button>
    <a class="jz-btn jz-btn--sm" href="?id=${encodeURIComponent(taskAgent(t))}">该 agent 对话</a>
    <button class="jz-btn jz-btn--sm" id="btnRefreshDetail">刷新</button>
    <button class="jz-btn jz-btn--sm jz-btn--ghost" id="btnCloseDetail">关闭</button>
    <span class="logbar__hint" id="detailMsg"></span>`;
  const cancelBtn = document.getElementById('btnCancelTask');
  if (cancelBtn) cancelBtn.onclick = async () => {
    cancelBtn.disabled = true;
    const r = await cancelTask(taskId(t));
    document.getElementById('detailMsg').textContent = r.ok ? '已请求取消' : '取消失败：' + r.error;
    loadDetail(taskId(t), true);
  };
  const delBtn = document.getElementById('btnDeleteTask');
  if (delBtn) delBtn.onclick = async () => {
    delBtn.disabled = true;
    const r = await deleteTaskRecords([taskId(t)], { running: isRunning(t) });
    if (!r.ok) {
      delBtn.disabled = false;
      if (!r.cancelled && window.JzUI) JzUI.toast(r.error || '删除失败', 'fail');
      return;
    }
    closeDetail();
    if (window.JzUI) JzUI.toast('已删除', 'ok');
    refreshListOnly();
  };
  document.getElementById('btnRefreshDetail').onclick = () => loadDetail(taskId(t), true);
  document.getElementById('btnCloseDetail').onclick = closeDetail;

  document.getElementById('detailEvents').innerHTML = eventHtml(taskEvents(t));
  document.getElementById('detailArtifacts').innerHTML = artifactHtml(t.artifacts);
  const hist = document.getElementById('detailHistory');
  hist.innerHTML = historyHtml(t.history);
  hist.style.display = (t.history && t.history.length) ? '' : 'none';
  const err = statusMessage(t);
  document.getElementById('detailBody').innerHTML = `
    <div class="kv"><span class="k">指令</span><span class="v"><pre class="jz-log" style="max-height:180px">${esc(taskPrompt(t))}</pre></span></div>
    <div class="kv"><span class="k">验证命令</span><span class="v">${meta(t).verifyCmd ? '<code>' + esc(meta(t).verifyCmd) + '</code>' : '<span class="muted">(无)</span>'}</span></div>
    <div class="kv"><span class="k">状态说明</span><span class="v">${err ? '<span style="color:var(--jz-fail)">' + esc(err) + '</span>' : '<span class="muted">(无)</span>'}</span></div>
    <div class="kv"><span class="k">agent 回复</span><span class="v"><pre class="jz-log" style="max-height:220px">${esc(replyText(t))}</pre></span></div>`;
}
async function loadDetail(id, fromStream) {
  if (DETAIL_ID !== id) { closeDetailWatch(); DETAIL_ID = id; CURRENT_TASK = null; }
  let t;
  try { t = await getJson(api('tasks/detail?id=' + encodeURIComponent(id))); }
  catch (e) {
    const box = document.getElementById('detail');
    if (box) box.classList.remove('hidden');
    const body = document.getElementById('detailBody');
    if (body) body.innerHTML = `<span style="color:var(--jz-fail)">读取失败: ${esc(e)}</span>`;
    return;
  }
  renderDetail(t);
  if (isTerminal(t)) { closeDetailWatch(); if (fromStream) refreshListOnly(); }
  else if (!fromStream && !ES) openStream(id);
}
function closeDetail() {
  closeDetailWatch();
  DETAIL_ID = ''; CURRENT_TASK = null;
  const box = document.getElementById('detail');
  if (box) box.classList.add('hidden');
  history.replaceState(null, '', 'tasks' + location.search);
}
/* project 就是工作区根目录; 下拉里显示末段目录名, value 是完整路径 */
function projName(p) {
  const s = String(p || '').replace(/[\\/]+$/, '');
  const i = Math.max(s.lastIndexOf('/'), s.lastIndexOf('\\'));
  return i >= 0 ? s.slice(i + 1) : s;
}
/* 按 project 过滤(服务端暂无该参数): 下拉里只列当前结果里出现过的 project,
   没选 = 全部任务。任务详情不受影响 —— 点进去看到的是这条任务自己的历史。 */
function filterByProject(list) {
  const sel = document.getElementById('fProject');
  if (!sel) return list;
  const projects = Array.from(new Set(list.map(t => (meta(t) || {}).project).filter(Boolean))).sort();
  const cur = sel.value;
  sel.innerHTML = '<option value="">全部</option>' +
    projects.map(p => `<option value="${esc(p)}" title="${esc(p)}">${esc(projName(p))}</option>`).join('');
  sel.value = projects.indexOf(cur) >= 0 ? cur : '';
  const pj = sel.value;
  return pj ? list.filter(t => (meta(t) || {}).project === pj) : list;
}
async function refreshListOnly() {
  try {
    const data = await getJson(api(listQuery()));
    document.getElementById('listBody').innerHTML = taskRows(filterByProject(data.tasks || []));
    bindTaskRows();
  } catch (e) { /* 列表刷新失败不影响详情 */ }
}
function listQuery() {
  const agent = document.getElementById('fAgent').value;
  const state = document.getElementById('fState').value;
  const contextId = document.getElementById('fContext').value.trim();
  const limit = document.getElementById('fLimit').value || 50;
  const all = document.getElementById('fAllOrigin') && document.getElementById('fAllOrigin').checked;
  return 'tasks?limit=' + encodeURIComponent(limit) +
    (agent ? '&agentId=' + encodeURIComponent(agent) : '') +
    (state ? '&state=' + encodeURIComponent(state) : '') +
    (contextId ? '&contextId=' + encodeURIComponent(contextId) : '') +
    (all ? '' : '&origin=user');
}
function bindTaskRows() {
  document.querySelectorAll('#listBody tr[data-task]').forEach(tr => {
    tr.onclick = () => {
      const id = tr.dataset.task;
      history.replaceState(null, '', 'tasks?id=' + encodeURIComponent(id));
      loadDetail(id);
    };
  });
  document.querySelectorAll('#listBody [data-del]').forEach(btn => {
    btn.onclick = async (ev) => {
      ev.preventDefault(); ev.stopPropagation();
      const id = btn.dataset.del;
      const r = await deleteTaskRecords([id]);
      if (r.cancelled) return;
      if (!r.ok) { if (window.JzUI) JzUI.toast(r.error || '删除失败', 'fail'); return; }
      if (DETAIL_ID === id) closeDetail();
      if (window.JzUI) JzUI.toast('已删除', 'ok');
      refreshListOnly();
    };
  });
}
async function loadTasksPage() {
  const fAgent = document.getElementById('fAgent');
  if (fAgent.options.length <= 1) {
    let ids = [];
    try { const s = await getJson(api('status')); ids = (s.known || s.online || []).map(a => a.agentId); }
    catch (e) { /* ignore */ }
    ids.forEach(id => fAgent.insertAdjacentHTML('beforeend', `<option value="${esc(id)}">${esc(id)}</option>`));
  }
  const url = new URL(location.href);
  if (url.searchParams.get('agentId')) fAgent.value = url.searchParams.get('agentId');
  if (url.searchParams.get('state')) document.getElementById('fState').value = url.searchParams.get('state');
  if (url.searchParams.get('contextId')) document.getElementById('fContext').value = url.searchParams.get('contextId');
  let data;
  try { data = await getJson(api(listQuery())); }
  catch (e) { document.getElementById('meta').textContent = '读取失败: ' + e; return; }
  const list = data.tasks || [];
  const shown = filterByProject(list);
  document.getElementById('meta').textContent =
    `共 ${data.totalSize || 0} 条记录，当前显示 ${shown.length} 条` +
    (shown.length !== list.length ? `（已按 project 筛选，本页 ${list.length} 条）` : '');
  document.getElementById('hint').textContent = '点一行看 Task 详情';
  document.getElementById('listBody').innerHTML = taskRows(shown);
  bindTaskRows();
  const id = url.searchParams.get('id');
  if (id) loadDetail(id);
}

/* ============================ 启动 ============================ */

if (PAGE === 'agents') {
  loadAgentsPage();
  const btn = document.getElementById('btnRefresh');
  if (btn) btn.onclick = loadAgentsPage;
} else if (PAGE === 'agent') {
  initChatPage();
} else if (PAGE === 'tasks') {
  loadTasksPage();
  ['fAgent', 'fState', 'fLimit', 'fContext', 'fProject', 'fAllOrigin'].forEach(key => {
    const el = document.getElementById(key);
    if (el) el.onchange = loadTasksPage;
  });
  document.getElementById('btnReload').onclick = loadTasksPage;
  document.getElementById('btnClose').onclick = closeDetail;
}
