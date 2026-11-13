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
/* 事件按页拿: 服务端只回最近 1000 条(起点对齐到 user_msg), 更早的点"加载更多"再翻。
   1000 条够绝大多数任务一次到底 —— 200 条时三四十个块就冒"加载更多", 看着像没显示全。
   翻过的部分缓存在本机, 轮询/刷新重新拉列表不会把它冲掉。 */
const AG_OLDER = {};        // taskId -> { events: [...], offset: n }
/* 工具块的输出文本: key(任务:序号) -> result。只给"复制"按钮用,
   每次都随内容重画, 所以直接当临时表使唤 —— 不要把长文本塞进 data-* 属性。 */
let TOOL_OUT = {};
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
/* 本次提交的耗时: 从最后一个 user_msg 到任务结束。
   一条任务可以被"接着说"续很多轮, 卡片上显示累计 costMs 会越滚越大, 看着像这次跑了 86s。 */
function taskCostLast(t) {
  const m = meta(t);
  const evs = taskEvents(t) || [];
  let start = 0;
  for (let i = evs.length - 1; i >= 0; i--) {
    if (evs[i] && evs[i].type === 'user_msg') { start = Number(evs[i].at) || 0; break; }
  }
  if (start && m.endMs && m.endMs >= start) return ((m.endMs - start) / 1000).toFixed(1) + 's';
  return taskCost(t);            // 老记录没有 user_msg 事件: 退回总耗时
}
/* 卡片底部那行。两个信息都只在"有内容"时才出现:
   - rounds 是**验证循环**的轮次(verify_rounds), 不是对话轮数: 不配验证脚本恒为 0,
     配了也要失败重试才涨 —— 写个 "0 轮" 等于占位, 还容易被当成"我说了几次话";
     涨起来了才值得说, 所以只在 >0 时显示成 "验证 N 轮"(说明模型返工了几次)。
   - 耗时只在结束后给, 执行中数字一直往上跳, 既不准也没参考价值。 */
function taskMetaText(t) {
  const out = [];
  const r = taskRounds(t);
  if (r > 0) out.push('验证 ' + r + ' 轮');
  const c = isRunning(t) ? '' : taskCostLast(t);
  if (c && c !== '-') out.push(c);
  return out.join(' · ');
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
/* 复制到剪贴板: 优先 Clipboard API(仅安全上下文有), 否则退回隐藏 textarea + execCommand。
   返回 Promise<bool> —— 调用方据此决定提示"已复制"还是失败, 不要吞掉。 */
async function copyText(s) {
  if (!s) return false;
  try {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      await navigator.clipboard.writeText(s);
      return true;
    }
  } catch (e) { /* 权限被拒/非安全上下文: 走降级 */ }
  try {
    const ta = document.createElement('textarea');
    ta.value = s;
    ta.setAttribute('readonly', '');
    ta.style.position = 'fixed';
    ta.style.top = '-1000px';
    ta.style.opacity = '0';
    document.body.appendChild(ta);
    ta.select();
    ta.setSelectionRange(0, s.length);
    const ok = document.execCommand('copy');
    document.body.removeChild(ta);
    return !!ok;
  } catch (e) {
    return false;
  }
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
async function sendMessage({ agentId, prompt, verifyCmd, contextId, taskId, rootDir, images }) {
  const body = {
    agentId,
    message: { role: 'ROLE_USER', parts: [{ text: prompt }] },
    returnImmediately: true
  };
  // 图片附件(多模态): data URL 数组, 后端 JZAgentCli::extractImages 直接认这个字段
  if (images && images.length) body.images = images;
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
  // 后端 retry 事件里字段名是 retry(不是 attempt); attempt 留着兼容旧记录
  else if (type === 'retry') s = '第 ' + (pick('retry') || pick('attempt') || '?') + ' 次重试 · ' + (pick('reason') || pick('error') || '');
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
      + (b.args ? ' <span class="muted cs-tool__args">' + esc(String(b.args).slice(0, 140)) + '</span>' : '');
    // 复制按钮: 贴着 summary 右侧, 悬停才出现(见 app.css .cs-tool__copy)。
    // 复制的是**输出**(result), 不带命令 —— 常见用途是把报错贴出去。
    if (b.result != null) TOOL_OUT[key] = b.result;
    const copy = b.result == null ? ''
      : `<button class="cs-tool__copy" type="button" data-copy="${esc(key)}" title="复制输出">复制</button>`;
    return `<details class="cs-tool" data-open="${esc(key)}"${attr}>
      <summary><span class="cs-tool__dot ${dot}"></span>${head}${copy}</summary>
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
  TOOL_OUT = {};                 // 整块重画, 输出表跟着重建
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
  const metaTxt = taskMetaText(t);
  return `<div class="cs-task" data-task="${esc(id)}">
    ${headBubble}
    <div class="cs-conv">${inner}</div>
    ${err ? `<div class="jz-msg jz-msg--error"><div class="jz-bubble">${esc(err)}</div></div>` : ''}
    <div class="jz-msg__meta cs-task__meta">
      <span class="jz-badge ${BADGE[stateOf(t)] || ''}">${esc(stateText(t))}</span>
      ${metaTxt ? `<span>${esc(metaTxt)}</span>` : ''}
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
  // 条目只占一行: 状态看圆点, 细节放 tooltip(执行中不报耗时, 同卡片)
  const cost = isRunning(t) ? '' : taskCost(t);
  const tip = prompt + '\n' + stateText(t)
    + (rounds ? ' · 验证 ' + rounds + ' 轮' : '') + (cost && cost !== '-' ? ' · ' + cost : '')
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
  if (scroll) activateTab('chat');
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
  // 工具块上的「复制」: 按钮在 <summary> 里, 不 stopPropagation 会顺带把折叠块开/关
  box.querySelectorAll('.cs-tool__copy').forEach(btn => {
    btn.onclick = async (e) => {
      e.preventDefault();
      e.stopPropagation();
      const txt = TOOL_OUT[btn.dataset.copy] || '';
      if (!txt) return;
      const ok = await copyText(txt);
      if (!ok) { if (window.JzUI) JzUI.toast('复制失败，浏览器不给写剪贴板', 'fail'); return; }
      const old = btn.textContent;
      btn.textContent = '已复制';
      btn.classList.add('is-done');
      setTimeout(() => { btn.textContent = old; btn.classList.remove('is-done'); }, 1200);
    };
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
    const d = await getJson(api('tasks/events?id=' + encodeURIComponent(id) + '&before=' + before + '&limit=1000'));
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
  // 对话上方只留"这条任务正在跑, 要不要停": 删除走左栏每条任务的 ×,
  // 全部任务走 /tasks 页面(不占对话的地方)。
  acts.innerHTML = `${running ? '<button class="jz-btn jz-btn--sm jz-btn--ghost" id="btnCancelTask" type="button">取消任务</button>' : ''}
    <button class="jz-btn jz-btn--sm jz-btn--ghost" id="btnRefresh" type="button">刷新</button>`;
  const c = document.getElementById('btnCancelTask');
  if (c) c.onclick = async () => {
    c.disabled = true;
    const r = await cancelTask(taskId(running));
    if (window.JzUI) JzUI.toast(r.ok ? '已请求取消' : '取消失败：' + r.error, r.ok ? 'warn' : 'fail');
    loadTasks();
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

/* ---- 目录选择器(仿 VSCode Quick Open): 一行输入 + 候选, 不搞路径栏/快捷区 ---- */
const DIRPICK_KEY = 'jz.dirpick.';
let DIR_TARGET = '';      // 这次选目录给哪台 agent 用(主机行的 + 可能是别的 agent)
let DIR_CUR = '';         // 输入框当前对应的有效目录('' = 不是个目录)
let DIR_ITEMS = [];       // [{path, name, note}]
let DIR_SEL = 0;
let DIR_TIMER = 0;
let DIR_ERR = '';         // 上次读目录失败的原因(显示在候选区, 别让人对着空列表猜)
const DIR_POOL = { loaded: false, items: [] };   // 盘符/主目录的第一层(按名字搜时用, 只拉一次)

function dirPickStart() {
  const id = DIR_TARGET || AG.id;
  // 默认落在"最近用过的目录": 上次在这个弹窗里选的 -> 当前工作目录 -> cli 根目录
  if (id === AG.id)
    return lsGet(DIRPICK_KEY + AG.id) || workdir() || (AG.info && AG.info.rootDir) || '';
  const h = (HOSTS.list || []).find(x => x.agentId === id);
  return (h && (h.rootDir || (h.dirs && h.dirs[0] && h.dirs[0].path))) || '';
}

async function openDirDlg(agentId) {
  const dlg = document.getElementById('dlgDir');
  if (!dlg) return;
  DIR_TARGET = agentId || AG.id;
  // 给别的 agent 选目录时默认值来自主机列表 —— 它可能还没加载过(没切过"主机"视图)
  if (DIR_TARGET !== AG.id && !(HOSTS && HOSTS.loaded)) {
    try { await loadHosts(); } catch (e) { /* 拿不到就退回默认目录 */ }
  }
  const inp = document.getElementById('fDirPath');
  if (inp) inp.value = dirPickStart();
  if (typeof dlg.showModal === 'function') { if (!dlg.open) dlg.showModal(); }
  else dlg.setAttribute('open', '');
  if (inp) { inp.focus(); inp.select(); }
  await refreshDirPick();
}

function closeDirDlg() {
  const dlg = document.getElementById('dlgDir');
  if (!dlg) return;
  if (typeof dlg.close === 'function') dlg.close(); else dlg.removeAttribute('open');
}

/* "C:/work/pro" -> {parent:'C:/work', frag:'pro'}
   "work"       -> {parent:'', frag:'work'}          纯名字: 在已知目录里搜
   "C:/work/"   -> {parent:'C:/work', frag:''}       列它的子目录 */
function splitDirQuery(raw) {
  if (!raw) return { parent: '', frag: '' };
  const last = raw.charAt(raw.length - 1);
  if (last === '/' || last === '\\')
    return { parent: raw.replace(/[\/\\]+$/, ''), frag: '' };
  const i = raw.lastIndexOf('/');
  if (i < 0) return { parent: '', frag: raw };
  let parent = raw.slice(0, i);
  if (/^[A-Za-z]:$/.test(parent)) parent += '/';   // "C:" -> "C:/"
  return { parent, frag: raw.slice(i + 1) };
}

function renderDirPickList() {
  const listEl = document.getElementById('dirList');
  if (!listEl) return;
  if (!DIR_ITEMS.length) {
    listEl.innerHTML = '<div class="dpick__empty">'
      + esc(DIR_ERR || '没有匹配的文件夹') + '</div>';
    return;
  }
  // 只显示文件夹名(路径在输入框里), 和系统那个 "Open Folder" 对话框一致
  listEl.innerHTML = DIR_ITEMS.map((it, i) =>
    `<button class="dpick-row${i === DIR_SEL ? ' is-sel' : ''}" type="button"
      data-i="${i}" title="${esc(it.path)}">${esc(it.name)}</button>`).join('');
  listEl.querySelectorAll('.dpick-row').forEach(b => {
    b.onclick = () => {
      const it = DIR_ITEMS[Number(b.dataset.i)];
      if (!it) return;
      if (it.up) { dirPickGoUp(it.path); return; }
      useDirPick(it.path);
    };
    b.onmouseenter = () => { DIR_SEL = Number(b.dataset.i); markDirSel(); };
  });
  markDirSel();
}

function markDirSel() {
  const listEl = document.getElementById('dirList');
  if (!listEl) return;
  const rows = listEl.querySelectorAll('.dpick-row');
  rows.forEach((b, i) => b.classList.toggle('is-sel', i === DIR_SEL));
  const sel = rows[DIR_SEL];
  if (sel && sel.scrollIntoView) sel.scrollIntoView({ block: 'nearest' });
}

/* 点 "..": 把输入框改成上一级并刷新(相当于"进入上一级目录", 不是确认它) */
function dirPickGoUp(parent) {
  const inp = document.getElementById('fDirPath');
  if (!inp) return;
  inp.value = String(parent || '').replace(/[\/\\]+$/, '') + '/';
  DIR_SEL = 0;
  refreshDirPick();
}

function moveDirSel(d) {
  if (!DIR_ITEMS.length) return;
  DIR_SEL = (DIR_SEL + d + DIR_ITEMS.length) % DIR_ITEMS.length;
  markDirSel();
}

/* 盘符 + 主目录的第一层, 一个会话只拉一次(名字搜索用; 别每次敲字都发一串请求) */
async function loadDirPool() {
  if (DIR_POOL.loaded) return DIR_POOL.items;
  DIR_POOL.loaded = true;
  const roots = [];
  try {
    const r = await getJson(api('fs'));
    (r.drives || []).forEach(d => roots.push(d));
    if (r.home) roots.push(r.home);
  } catch (e) { return DIR_POOL.items; }

  const seen = {};
  for (const p of roots) {
    try {
      const j = await getJson(api('fs') + '?path=' + encodeURIComponent(p));
      (j.dirs || []).forEach(d => {
        if (!d || !d.path || seen[d.path]) return;
        seen[d.path] = 1;
        DIR_POOL.items.push({ path: d.path, name: d.name || projName(d.path) });
      });
    } catch (e) { /* 某个盘读不了就跳过 */ }
  }
  return DIR_POOL.items;
}

/* 按输入框内容刷新候选:
   ① 输入本身是有效目录 -> 第一项"打开它"
   ② 该目录下的子文件夹(按最后一段过滤)
   ③ 输入不带路径时, 再把这台 agent 用过的历史目录按名字过滤一遍 */
async function refreshDirPick() {
  const inp = document.getElementById('fDirPath');
  const listEl = document.getElementById('dirList');
  if (!inp || !listEl) return;

  const raw = inp.value.trim().replace(/\\/g, '/');
  // 输入框全空: 不给任何候选(以前会退化成"列默认目录", 看起来像空查询也能匹配)
  if (!raw) {
    DIR_CUR = '';
    DIR_ERR = '';
    DIR_ITEMS = [];
    DIR_SEL = 0;
    renderDirPickList();
    return;
  }
  const q = splitDirQuery(raw);

  // ⚠ getJson 直接返回响应体(不是 {ok,json}); 失败是抛异常 —— 早期代码按 {ok,json}
  //   解, 拿到的一直是 undefined, 所以列表永远是空的(点"打开"也就没反应)。
  let j = null, why = '';
  try { j = await getJson(api('fs') + (q.parent ? '?path=' + encodeURIComponent(q.parent) : '')); }
  catch (e) { j = null; why = (e && e.message) ? e.message : '读不到目录'; }
  const ok = !!j;
  DIR_CUR = ok ? (j.path || q.parent || '') : '';
  DIR_ERR = ok ? '' : why;

  const f = q.frag.toLowerCase();
  const items = [];
  const seen = {};
  if (ok && j.parent) items.push({ path: j.parent, name: '..', up: true });   // 上一级

  const push = (path, name) => {
    if (!path || seen[path]) return;
    const nm = String(name == null ? '' : name);
    // 只按"文件夹名"过滤, 不拿整条路径比 —— 否则输入 work 会把 .../workspace/... 全捞进来
    if (f && nm.toLowerCase().indexOf(f) < 0) return;
    seen[path] = 1;
    items.push({ path, name: nm || projName(path) });
  };

  if (ok && DIR_CUR) push(DIR_CUR, projName(DIR_CUR));
  (ok ? (j.dirs || []) : []).forEach(d => push(d.path, d.name));

  if (!q.parent) {
    // 不带路径时（输入 "work" / "/work"）在"已知目录"里搜, 而不是去递归扫盘:
    //   ① 这台 agent 用过的目录
    //   ② 盘符 + 主目录的第一层（这样 C:/work 这种顶层目录能被 work 命中）
    const target = DIR_TARGET || AG.id;
    (HOSTS.list || []).forEach(h => {
      if (h.agentId !== target) return;
      (h.dirs || []).forEach(d => push(d.path, projName(d.path)));
    });
    if (f) {
      const pool = await loadDirPool();
      pool.forEach(d => push(d.path, d.name));
    }
  }

  DIR_ITEMS = items;
  // 默认高亮第一个真实目录(不选 ".."), 免得一按回车就往上跳
  DIR_SEL = Math.max(0, items.findIndex(x => !x.up));
  renderDirPickList();
}

/* 打开这个目录: 当前 agent 切工作区; 别的 agent 就跳过去(?id=&dir=) */
function useDirPick(path) {
  const target = DIR_TARGET || AG.id;
  if (!path) return;
  lsSet(DIRPICK_KEY + target, path);
  closeDirDlg();
  if (target === AG.id) {
    setWorkdir(path);
    if (window.JzUI) JzUI.toast('工作目录：' + path, 'ok');
    return;
  }
  location.href = '?id=' + encodeURIComponent(target) + '&dir=' + encodeURIComponent(path);
}

function useDirDlg() { useDirPick(DIR_ITEMS[DIR_SEL] ? DIR_ITEMS[DIR_SEL].path : DIR_CUR); }


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
  AG.kind = d.kind || '';
  AG.id = AG.info.agentId || want;
  // 从「主机」视图点别的 agent 的目录跳过来: 先落到本机记录, 再把 dir 从地址栏抹掉
  const u0 = new URL(location.href);
  const dq = u0.searchParams.get('dir');
  if (dq && isAbsPath(dq)) {
    lsSet(WD_KEY + AG.id, dq);
    u0.searchParams.delete('dir');
    history.replaceState(null, '', u0);
    // 既然是在"打开某个目录"的动作里跳过来的, 落地就该是那个目录的文件列表,
    // 不是对话页 —— 否则还要再点一次 rail 的文件图标
    setSideView('files');
  }

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

  renderHead(); renderTask(false, scrollMode); renderSide(); syncComposer();
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

/* ---------------- 附图（多模态） ----------------
   上限与后端 JZAgentCli::extractImages 对齐: 8 张 / 单张 8M 字符(data URL)。
   前端先拦一道, 省得几 MB 的图传完才被后端 400 打回。 */
const IMG_MAX = 8;
const IMG_MAX_CHARS = 8 * 1024 * 1024;
let PENDING_IMGS = [];      // [{name, dataUrl, w, h, bytes}]

function fmtBytes(n) {
  if (!n) return '未知大小';
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return Math.round(n / 1024) + ' KB';
  return (n / 1024 / 1024).toFixed(1) + ' MB';
}

/* 输入框内的缩略图条: 图 + 右上角删除 + 悬停详情(文件名/尺寸/大小) */
function renderImgs() {
  const box = document.getElementById('imgList');
  if (!box) return;
  if (!PENDING_IMGS.length) {
    box.innerHTML = '';
    box.classList.add('hidden');
    return;
  }
  box.classList.remove('hidden');
  box.innerHTML = PENDING_IMGS.map((im, i) => `
    <div class="cs-img">
      <img class="cs-img__mx" src="${im.dataUrl}" alt="">
      <button class="cs-img__x" type="button" data-del="${i}" title="移除">×</button>
      <div class="cs-img__tip">${esc(im.name)}<br>${im.w || '?'}×${im.h || '?'} · ${fmtBytes(im.bytes)}</div>
    </div>`).join('');
}

function clearImgs() {
  PENDING_IMGS = [];
  renderImgs();
}

function readImageFile(file) {
  return new Promise((resolve, reject) => {
    const fr = new FileReader();
    fr.onerror = () => reject(new Error('读取失败'));
    fr.onload = () => {
      const dataUrl = String(fr.result || '');
      const probe = new Image();
      const done = (w, h) => resolve({
        name: file.name || '粘贴的图片', dataUrl, w, h, bytes: file.size || 0
      });
      probe.onload = () => done(probe.naturalWidth, probe.naturalHeight);
      probe.onerror = () => done(0, 0);
      probe.src = dataUrl;
    };
    fr.readAsDataURL(file);
  });
}

async function addImageFiles(files) {
  const msg = document.getElementById('msg');
  const picked = Array.from(files || []).filter(f => f && /^image\//.test(f.type || ''));
  if (!picked.length) { msg.textContent = '只能加图片文件'; return; }
  for (const f of picked) {
    if (PENDING_IMGS.length >= IMG_MAX) { msg.textContent = `最多 ${IMG_MAX} 张图`; break; }
    try {
      const im = await readImageFile(f);
      if (im.dataUrl.length > IMG_MAX_CHARS) {
        msg.textContent = `「${im.name}」太大（超过 8MB），压缩后再试`;
        continue;
      }
      PENDING_IMGS.push(im);
      msg.textContent = '';
    } catch (e) {
      msg.textContent = '有一张图没读出来：' + (e && e.message ? e.message : e);
    }
  }
  renderImgs();
}

let SUBMIT_LOCK = false;
async function doSend() {
  const ta = document.getElementById('fPrompt');
  const msg = document.getElementById('msg');
  const text = ta.value.trim();
  const imgs = PENDING_IMGS.map(i => i.dataUrl);
  // 只发图不发字也允许（后端 extractImages 支持 prompt 为空）
  if (!text && !imgs.length) { msg.textContent = '先写点要它做的事，或加一张图片'; ta.focus(); return; }
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
      images: imgs,
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
  clearImgs();                                   // 附件已经随请求发出去了, 清空待发队列
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

/* ---- 主区 tab ----
   「对话」常驻且不可关; 点左侧文件树里的文件就多开一个, 标题是文件名。
   tab 内容存在对象里, 切回来直接塞进 DOM —— chat 那一屏的 DOM **从不销毁**
   (几十条消息、滚动位置、正在拼的流式文本都在里面), 重建一次就全废了。 */
/* 这一屏叫「会话」: 它本质就是一次和 agent 的会话(多轮指令 + 它的思考/工具/回复),
   「对话」太口语, 「任务」又只剩派活那一下, 丢了来回交流的意思。
   为了不重名, 侧栏那个列队列表的视图改叫「任务」—— 那里面本来就是一条条任务。 */
const ED = { list: [{ id: 'chat', kind: 'chat', title: '会话' }], act: 'chat' };
const edById = (id) => ED.list.filter(t => t.id === id)[0];
const edFileId = (p) => 'f:' + p;

function renderTabs() {
  const box = document.getElementById('tabs');
  if (!box) return;
  box.innerHTML = ED.list.map(t => `<button class="tab${t.id === ED.act ? ' is-on' : ''}"
      type="button" data-tab-id="${esc(t.id)}" title="${esc(t.path || t.title)}"
      ><span class="tab__t">${esc(t.title)}</span>${t.kind === 'chat' ? ''
        : `<span class="tab__x" data-close="${esc(t.id)}" title="关闭">×</span>`}</button>`).join('');
  box.querySelectorAll('button[data-tab-id]').forEach(b => {
    b.onclick = (ev) => {
      const x = ev.target && ev.target.closest ? ev.target.closest('[data-close]') : null;
      if (x) { ev.stopPropagation(); closeTab(x.getAttribute('data-close')); return; }
      activateTab(b.getAttribute('data-tab-id'));
    };
  });
}
function activateTab(id) {
  const t = edById(id) || ED.list[0];
  ED.act = t.id;
  // 两个内容区互斥: 对话 / 文件
  document.querySelectorAll('.pane').forEach(p => {
    const on = t.kind === 'chat' ? p.dataset.pane === 'chat' : p.dataset.pane === 'preview';
    p.classList.toggle('hidden', !on);
  });
  if (t.kind === 'file') paintFileTab(t);
  renderTabs();
}
function closeTab(id) {
  const i = ED.list.findIndex(t => t.id === id);
  if (i < 0 || ED.list[i].kind === 'chat') return;      // 对话不给关
  ED.list.splice(i, 1);
  if (ED.act === id) activateTab((ED.list[i] || ED.list[i - 1] || ED.list[0]).id);
  else renderTabs();
}
/* 语法高亮: 语言按扩展名给, **不用** hljs 的自动检测 —— 它会把 .h 猜成 C、
   短 Python 猜成 Ruby, 这些文件恰恰是我们天天看的。 */
const HLJS_LANG = {
  js: 'javascript', mjs: 'javascript', cjs: 'javascript', jsx: 'javascript',
  ts: 'typescript', tsx: 'typescript',
  cpp: 'cpp', cc: 'cpp', cxx: 'cpp', hpp: 'cpp', hh: 'cpp', h: 'cpp', inl: 'cpp',
  c: 'c',
  py: 'python', pyw: 'python',
  json: 'json', css: 'css', scss: 'scss', less: 'less',
  html: 'xml', htm: 'xml', xml: 'xml', svg: 'xml', vue: 'xml',
  md: 'markdown', markdown: 'markdown',
  sh: 'bash', bash: 'bash', zsh: 'bash', bat: 'dos', cmd: 'dos', ps1: 'powershell',
  yml: 'yaml', yaml: 'yaml', toml: 'ini', ini: 'ini', conf: 'ini',
  sql: 'sql', go: 'go', rs: 'rust', java: 'java', lua: 'lua', php: 'php', rb: 'ruby',
};
/* 超过这个尺寸就不高亮: hljs 是同步的, 几百 KB 的文件会明显卡住切 tab */
const HLJS_MAX = 200 * 1024;

function fileExt(path) {
  const s = String(path || '');
  const i = s.lastIndexOf('.');
  return i < 0 ? '' : s.slice(i + 1).toLowerCase();
}

/* 行号: 在每行开头插一个 <span class="prev-ln">, 数字由 CSS 计数器打出来。
   为什么不用"按行 split 再包一层": hljs 的 token <span> 会跨行(多行字符串/注释),
   按文本切会切断标签, 结构就烂了。插占位 span 不碰已有标签, 安全。 */
function withLineNumbers(html) {
  const s = String(html || '').replace(/\r\n/g, '\n');
  return '<span class="prev-ln"></span>' + s.split('\n').join('\n<span class="prev-ln"></span>');
}

function paintFileTab(t) {
  const pp = document.getElementById('prevPath');
  const meta = document.getElementById('prevMeta');
  const body = document.getElementById('prevBody');
  const copy = document.getElementById('btnPrevCopy');
  if (pp) { pp.textContent = t.path; pp.title = t.path; }
  if (meta) meta.textContent = t.note || '';
  if (body) {
    // 行号列宽度 = 最长行号的位数(单位 ch): 800 行就别按 4 位数的宽度留白
    const whole = String(t.text || '').replace(/\r\n/g, '\n').split('\n').length;
    body.style.setProperty('--ln-w', Math.max(2, String(whole).length) + 'ch');
    const lang = HLJS_LANG[fileExt(t.path)];
    if (window.hljs && lang && t.text && t.text.length <= HLJS_MAX) {
      // hljs 自己会把 <>& 转义掉, 这里可以直接 innerHTML; 语言名来自白名单
      body.innerHTML = withLineNumbers(
        hljs.highlight(t.text, { language: lang, ignoreIllegals: true }).value);
      body.classList.add('hljs');
    } else {
      body.innerHTML = withLineNumbers(esc(t.text || ''));
      body.classList.remove('hljs');
    }
  }
  if (copy) copy.disabled = !t.text;
}
/* ---- 新建文件夹 / 新建文件 ----
   边界在服务端判(与 cli 的区域模型一致: 工作区可写、外面只读) —— server 不知道
   网页当前切到哪个目录(那是浏览器的 localStorage), 所以 root 由这里带上去。 */
const NEW = { mode: 'create', dir: '', kind: 'dir', path: '' };

function openNewDlg(kind) {
  const dlg = document.getElementById('dlgNew');
  if (!dlg) return;
  NEW.mode = 'create';
  NEW.dir = CTX.path;
  NEW.kind = kind;
  const isDir = kind === 'dir';
  document.getElementById('dlgNewTitle').textContent = isDir ? '新建文件夹' : '新建文件';
  document.getElementById('lblNewName').textContent = isDir ? '文件夹名' : '文件名';
  document.getElementById('newNote').textContent = '在 ' + relPath(NEW.dir) + ' 下创建';
  const inp = document.getElementById('fNewName');
  inp.value = '';
  inp.placeholder = isDir ? '例如 components' : '例如 notes.md';
  if (typeof dlg.showModal === 'function') { if (!dlg.open) dlg.showModal(); }
  else dlg.setAttribute('open', '');
  setTimeout(() => inp.focus(), 30);
}

/* 重命名: 复用同一个输入框, 预填当前名字(不用 prompt —— 它会被拦、样式也不受控) */
function openRenameDlg(path) {
  const dlg = document.getElementById('dlgNew');
  if (!dlg) return;
  NEW.mode = 'rename';
  NEW.path = path;
  document.getElementById('dlgNewTitle').textContent = '重命名';
  document.getElementById('lblNewName').textContent = '新名字';
  document.getElementById('newNote').textContent = relPath(path);
  const inp = document.getElementById('fNewName');
  inp.value = baseName(path);
  inp.placeholder = '新名字';
  if (typeof dlg.showModal === 'function') { if (!dlg.open) dlg.showModal(); }
  else dlg.setAttribute('open', '');
  setTimeout(() => { inp.focus(); inp.select(); }, 30);
}

function closeNewDlg() {
  const dlg = document.getElementById('dlgNew');
  if (!dlg) return;
  if (typeof dlg.close === 'function') { if (dlg.open) dlg.close(); }
  else dlg.removeAttribute('open');
}

/* 路径的父目录(前端侧, 与服务端 parentDirOf 同规则) */
function dirOf(p) {
  const s = String(p || '').replace(/\\/g, '/').replace(/\/+$/, '');
  const i = s.lastIndexOf('/');
  if (i < 0) return '';
  if (i === 2 && s[1] === ':') return s.slice(0, 3);
  return s.slice(0, i) || '/';
}

/* 作废某个目录的缓存再重画(增删改之后都要走一遍, 否则树上还是旧的) */
async function refreshTreeDir(dir) {
  if (dir) {
    delete FS.nodes[dir];
    await fsLoad(dir);
  }
  fsRender();
}

async function submitNew() {
  const inp = document.getElementById('fNewName');
  const name = (inp.value || '').trim();
  if (!name) {
    if (window.JzUI) JzUI.toast('名字不能为空', 'warn');
    inp.focus();
    return;
  }
  const btn = document.getElementById('btnNewOk');
  btn.disabled = true;
  try {
    if (NEW.mode === 'rename') {
      const old = NEW.path;
      const r = await postJson(api('fs/rename'), { root: workdir(), path: old, name: name });
      if (!r.ok) {
        if (window.JzUI) JzUI.toast((r.json && r.json.error) || '重命名失败', 'fail');
        return;
      }
      closeNewDlg();
      FS.sel = (r.json && r.json.path) || old;
      await refreshTreeDir(dirOf(old));
      if (window.JzUI) JzUI.toast('已重命名为 ' + name, 'ok');
      return;
    }
    const parent = NEW.dir;
    const kind = NEW.kind;
    const r = await postJson(api('fs/create'), {
      root: workdir(), dir: parent, name: name, kind: kind,
    });
    if (!r.ok) {
      if (window.JzUI) JzUI.toast((r.json && r.json.error) || '创建失败', 'fail');
      return;
    }
    closeNewDlg();
    const made = (r.json && r.json.path) || (parent + '/' + name);
    // 父目录缓存作废再重载; 新建的是目录就顺手展开它, 不展开会以为没建成
    delete FS.nodes[parent];
    await fsLoad(parent);
    if (kind === 'dir') {
      FS.open[made] = true;
      await fsLoad(made);
    }
    fsRender();
    if (window.JzUI) JzUI.toast('已创建 ' + name, 'ok');
  } catch (e) {
    if (window.JzUI) JzUI.toast('操作失败：' + (e && e.message ? e.message : e), 'fail');
  } finally {
    btn.disabled = false;
  }
}

/* 删除: 目录是**递归**删, 所以必须确认; 工作区根目录服务端也不让删 */
async function deleteEntry(path, isDir) {
  const name = baseName(path);
  const what = isDir ? ('目录「' + name + '」及其全部内容') : ('文件「' + name + '」');
  let ok = false;
  if (window.JzUI && JzUI.confirm) {
    ok = await JzUI.confirm('确定删除' + what + '？此操作不可撤销。',
                            { title: '删除', danger: true, okText: '删除' });
  } else {
    ok = window.confirm('确定删除' + what + '？');
  }
  if (!ok) return;
  try {
    const r = await postJson(api('fs/delete'), { root: workdir(), path: path });
    if (!r.ok) {
      if (window.JzUI) JzUI.toast((r.json && r.json.error) || '删除失败', 'fail');
      return;
    }
    FS.sel = '';
    delete FS.open[path];
    await refreshTreeDir(dirOf(path));
    if (window.JzUI) JzUI.toast('已删除 ' + name, 'ok');
  } catch (e) {
    if (window.JzUI) JzUI.toast('删除失败：' + (e && e.message ? e.message : e), 'fail');
  }
}

/* 点文件树里的文件: 已经开着就切过去, 没开过才新建一个 tab */
async function openFileTab(path) {
  const id = edFileId(path);
  if (edById(id)) { activateTab(id); return; }
  const seg = String(path).replace(/[\\/]+$/, '').split(/[\\/]/);
  const t = { id, kind: 'file', path, title: seg[seg.length - 1] || path, text: '', note: '读取中…' };
  ED.list.push(t);
  activateTab(id);

  const show = () => { if (ED.act === id) paintFileTab(t); };
  let r = null;
  try { r = await fetchT(withToken(api('fs/raw') + '?path=' + encodeURIComponent(path)), null, 15000); }
  catch (e) { t.note = '读取失败：' + netReason(e); show(); return; }
  if (!r || !r.ok) {
    let msg = 'HTTP ' + (r ? r.status : '?');
    try { msg = await r.text(); } catch (e) {}
    t.note = msg; show(); return;
  }
  const size = Number(r.headers.get('X-File-Size') || 0);
  const bytes = new Uint8Array(await r.arrayBuffer());
  const dec = decodeBytes(bytes);
  if (dec.binary) { t.note = '二进制文件 · ' + fmtSize(size); show(); return; }
  t.text = dec.text;
  const truncated = size > bytes.length;
  t.note = `${dec.encoding.toUpperCase()} · ${fmtSize(size)}`
    + (truncated ? ' · 已截断（只显示前 1MB）' : '');
  show();
}

/* 点弹窗背景关闭: 必须"按在背景 + 抬在背景"才算。
   只判 click 的 target 不够 —— 在弹窗里框选文字、把鼠标拖到外面再松开时,
   click 会被派发到"按下/抬起的共同祖先"(也就是 dialog 本身), 弹窗就被误关了。 */
function bindBackdropClose(dlg, closeFn) {
  if (!dlg) return;
  let downOnBackdrop = false;
  dlg.addEventListener('mousedown', (e) => { downOnBackdrop = (e.target === dlg); });
  dlg.addEventListener('click', (e) => {
    if (downOnBackdrop && e.target === dlg) closeFn();
    downOnBackdrop = false;
  });
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
  bindBackdropClose(document.getElementById('dlgSettings'), closeSettings);

  // 工作目录选择(仿 VSCode Quick Open: 输入即筛选, ↑↓/Enter/Esc)
  on('btnDirUse', useDirDlg);
  const fDirPath = document.getElementById('fDirPath');
  if (fDirPath) {
    fDirPath.addEventListener('input', () => {
      clearTimeout(DIR_TIMER);
      DIR_TIMER = setTimeout(refreshDirPick, 160);
    });
    fDirPath.addEventListener('keydown', (e) => {
      if (e.isComposing || e.keyCode === 229) return;   // 输入法组字中的回车不算
      if (e.key === 'ArrowDown') { e.preventDefault(); moveDirSel(1); return; }
      if (e.key === 'ArrowUp') { e.preventDefault(); moveDirSel(-1); return; }
      if (e.key !== 'Enter') return;
      e.preventDefault();
      const it = DIR_ITEMS[DIR_SEL];
      if (it) useDirPick(it.path);
      else if (DIR_CUR) useDirPick(DIR_CUR);
    });
  }
  bindBackdropClose(document.getElementById('dlgDir'), closeDirDlg);

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
  on('btnPrevCopy', () => {
    const t = edById(ED.act);
    if (!t || !t.text || !navigator.clipboard) return;
    navigator.clipboard.writeText(t.text)
      .then(() => { if (window.JzUI) JzUI.toast('已复制', 'ok'); })
      .catch(() => {});
  });

  // ---- 附图：按钮 / 选择文件 / 粘贴截图 / 拖进来 ----
  const fImg = document.getElementById('fImg');
  on('btnImg', () => { if (fImg) fImg.click(); });
  if (fImg) {
    fImg.addEventListener('change', () => {
      addImageFiles(fImg.files);
      fImg.value = '';          // 同一张图连选两次也要能触发 change
    });
  }
  const imgBox = document.getElementById('imgList');
  if (imgBox) {
    imgBox.addEventListener('click', (e) => {
      const b = e.target.closest('[data-del]');
      if (!b) return;
      PENDING_IMGS.splice(Number(b.dataset.del), 1);
      renderImgs();
    });
  }
  const composerBox = document.querySelector('.jz-composer__box');
  if (composerBox) {
    composerBox.addEventListener('dragover', (e) => { e.preventDefault(); });
    composerBox.addEventListener('drop', (e) => {
      const files = e.dataTransfer && e.dataTransfer.files;
      if (files && files.length) { e.preventDefault(); addImageFiles(files); }
    });
  }

  const ta = document.getElementById('fPrompt');
  ta.addEventListener('paste', (e) => {
    const items = (e.clipboardData && e.clipboardData.items) || [];
    const files = [];
    for (const it of items) {
      if (it.kind === 'file' && /^image\//.test(it.type)) {
        const f = it.getAsFile();
        if (f) files.push(f);
      }
    }
    if (files.length) { e.preventDefault(); addImageFiles(files); }
  });
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
    activateTab('chat');
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

  // 文件树右键菜单: 点条目执行, 点别处 / Esc / 滚动都关掉
  const ctxMenu = document.getElementById('ctxMenu');
  if (ctxMenu) {
    ctxMenu.querySelectorAll('button[data-act]').forEach(b => {
      b.onclick = (ev) => {
        ev.stopPropagation();
        const act = b.getAttribute('data-act');
        if (act === 'copy') copyPath(CTX.path, '路径');
        else if (act === 'copy-rel') copyPath(relPath(CTX.path), '相对路径');
        else if (act === 'copy-name') copyPath(baseName(CTX.path), '名称');
        else if (act === 'new-dir') openNewDlg('dir');
        else if (act === 'new-file') openNewDlg('file');
        else if (act === 'rename') openRenameDlg(CTX.path);
        else if (act === 'delete') deleteEntry(CTX.path, CTX.isDir);
        hideCtxMenu();
      };
    });
    document.addEventListener('click', (ev) => {
      if (!ctxMenu.contains(ev.target)) hideCtxMenu();
    });
    document.addEventListener('keydown', (ev) => { if (ev.key === 'Escape') hideCtxMenu(); });
    window.addEventListener('resize', hideCtxMenu);
    // 菜单是 fixed 的, 不跟内容滚: 任何容器一滚就关掉
    document.addEventListener('scroll', hideCtxMenu, true);
  }

  // F2 = 重命名当前选中的那一项(文件管理器里的通行手势)
  document.addEventListener('keydown', (ev) => {
    if (ev.key !== 'F2') return;
    const tag = (ev.target && ev.target.tagName) || '';
    if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;
    const path = FS.sel || (CTX.path || '');
    if (!path) return;
    ev.preventDefault();
    openRenameDlg(path);
  });

  // 新建文件夹/文件 的输入框
  const dlgNew = document.getElementById('dlgNew');
  if (dlgNew) {
    document.getElementById('btnNewOk').onclick = submitNew;
    document.getElementById('btnNewCancel').onclick = closeNewDlg;
    document.getElementById('btnNewClose').onclick = closeNewDlg;
    document.getElementById('fNewName').onkeydown = (ev) => {
      if (ev.key === 'Enter') { ev.preventDefault(); submitNew(); }
      else if (ev.key === 'Escape') { ev.preventDefault(); closeNewDlg(); }
    };
  }

  activateTab('chat');
  // 老书签带 #log / #card: 没有对应面板了, 一律回落到对话
  window.addEventListener('hashchange', () => { if (location.hash === '#chat') activateTab('chat'); });
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
const SIDE_TITLE = { files: '文件', hosts: '主机', chats: '任务' };
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
const FS = { root: '', open: {}, nodes: {}, collapsed: false, sel: '' };

/* 记"当前选中的那一项": F2 和右键都靠它定位(没有多选, 就是最近点过的那行) */
function markTreeSel(btn) {
  document.querySelectorAll('#fsTree .fs-row.is-sel').forEach(x => x.classList.remove('is-sel'));
  btn.classList.add('is-sel');
  FS.sel = btn.dataset.file || btn.dataset.dir || '';
  FS.selIsDir = !!btn.dataset.dir;
}

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

/* ---- 文件树右键菜单 ----
   不用浏览器原生菜单(在文件树上尽是"审查元素/另存为"), 换成只用得上的几项。
   全局只放一个节点, 每次右键把目标路径记下来再摆位置。 */
const CTX = { path: '', isDir: false };

function showCtxMenu(ev, path, isDir) {
  ev.preventDefault();
  const m = document.getElementById('ctxMenu');
  if (!m) return;
  CTX.path = path || '';
  CTX.isDir = !!isDir;
  // "新建"只对目录有意义(在文件上新建是没道理的)
  m.querySelectorAll('[data-act="new-dir"],[data-act="new-file"],[data-sep]').forEach(el => {
    if (CTX.isDir) el.removeAttribute('hidden');
    else el.setAttribute('hidden', '');
  });
  // 先量尺寸再定位: 靠右下角右键时会超出视口, 往回收
  m.classList.remove('hidden');
  m.style.left = '0px'; m.style.top = '0px';
  const r = m.getBoundingClientRect();
  const x = Math.min(ev.clientX, window.innerWidth - r.width - 8);
  const y = Math.min(ev.clientY, window.innerHeight - r.height - 8);
  m.style.left = Math.max(8, x) + 'px';
  m.style.top = Math.max(8, y) + 'px';
}

function hideCtxMenu() {
  const m = document.getElementById('ctxMenu');
  if (m) m.classList.add('hidden');
}

/* 相对路径: 相对当前工作区根(派活时写相对路径比绝对路径短, 模型也更好读) */
function relPath(p) {
  const root = String(workdir() || '').replace(/[\/\\]+$/, '');
  const s = String(p || '');
  if (root && (s === root || s.indexOf(root + '/') === 0 || s.indexOf(root + '\\') === 0))
    return s.slice(root.length + 1).replace(/\\/g, '/') || '.';
  return s.replace(/\\/g, '/');
}

function baseName(p) {
  const s = String(p || '').replace(/[\/\\]+$/, '');
  const i = Math.max(s.lastIndexOf('/'), s.lastIndexOf('\\'));
  return i < 0 ? s : s.slice(i + 1);
}

/* 复制路径(文件树右键用): 派活/命令行里要的常常是绝对路径,
   从页面上右键拿走比手抄快。cliboard 不可用时退回 execCommand。 */
function copyPath(p, label) {
  const s = String(p || '');
  if (!s) return;
  const done = (ok) => {
    if (window.JzUI) JzUI.toast(ok ? '已复制' + (label || '路径') : '复制失败(剪贴板不可用)', ok ? 'ok' : 'fail');
  };
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(s).then(() => done(true)).catch(() => done(false));
    return;
  }
  try {
    const ta = document.createElement('textarea');
    ta.value = s;
    ta.style.position = 'fixed'; ta.style.opacity = '0';
    document.body.appendChild(ta);
    ta.select();
    const ok = document.execCommand('copy');
    document.body.removeChild(ta);
    done(ok);
  } catch (e) { done(false); }
}

function fsRender() {
  const box = document.getElementById('fsTree');
  if (!box) return;
  if (!FS.root) { box.innerHTML = '<div class="jz-empty">还没有工作区</div>'; return; }
  box.innerHTML = fsRows(FS.root, 0);
  box.querySelectorAll('button[data-dir]').forEach(b => {
    b.onclick = async () => {
      const path = b.dataset.dir;
      markTreeSel(b);
      if (FS.open[path]) { delete FS.open[path]; fsRender(); return; }
      FS.open[path] = true;
      fsRender();
      if (!FS.nodes[path]) { await fsLoad(path); fsRender(); }
    };
    b.oncontextmenu = (ev) => { markTreeSel(b); showCtxMenu(ev, b.dataset.dir, true); };
  });
  box.querySelectorAll('button[data-file]').forEach(b => {
    b.onclick = () => { markTreeSel(b); openFileTab(b.dataset.file); };
    // 右键 = 打开菜单(新建/重命名/删除/复制路径), 浏览器自己的菜单挡掉
    b.oncontextmenu = (ev) => { markTreeSel(b); showCtxMenu(ev, b.dataset.file, false); };
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
    out += `<button class="fs-row" type="button" data-dir="${esc(d.path)}" style="padding-left:${ind}px"
      title="${esc(d.path)}&#10;右键复制路径">
      <span class="fs-row__ico">${open ? '▾' : '▸'}</span>
      <span class="fs-row__name">📁 ${esc(d.name)}</span></button>`;
    if (open) out += fsRows(d.path, depth + 1);
  });
  n.files.forEach(f => {
    out += `<button class="fs-row" type="button" data-file="${esc(f.path)}" style="padding-left:${ind + 14}px"
      title="${esc(f.path)}&#10;右键复制路径">
      <span class="fs-row__ico">📄</span>
      <span class="fs-row__name">${esc(f.name)}</span>
      <span class="fs-row__meta">${fmtSize(f.size)}</span></button>`;
  });
  return out;
}

/* ---- 文件内容(只读): 后端给原始字节, 这里判编码 ---- */
function decodeBytes(bytes) {
  if (!bytes.length) return { text: '', encoding: 'utf-8' };
  for (let i = 0; i < bytes.length && i < 8192; i++)
    if (bytes[i] === 0) return { binary: true };          // 含 \0 当二进制
  try { return { text: new TextDecoder('utf-8', { fatal: true }).decode(bytes), encoding: 'utf-8' }; }
  catch (e) { /* 不是合法 UTF-8 */ }
  try { return { text: new TextDecoder('gb18030').decode(bytes), encoding: 'gb18030' }; }
  catch (e) { return { binary: true }; }
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
      </button>
      <button class="host-add" type="button" data-agent="${esc(h.agentId)}"
        title="在 ${esc(h.agentId)} 上打开文件夹（新目录 = 新会话的工作目录）">＋</button>
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
  // 主机行右侧的 ＋：不展开也能直接打开文件夹（别的 agent 选完会跳过去）
  box.querySelectorAll('.host-add').forEach(b => {
    b.onclick = (ev) => { ev.stopPropagation(); openDirDlg(b.dataset.agent); };
  });
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
