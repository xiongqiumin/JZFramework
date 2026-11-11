/* JZAgent 管理台 —— 只有一套模型: A2A v1.0
    Task{ id, contextId, status{state,message,timestamp}, artifacts[], history[], metadata{} }
    /              Agent 台账 + 进行中 Task
    /new           SendMessage(returnImmediately:true) → 一个 Task
    /tasks         ListTasks + GetTask / SubscribeToTask(事件、产物、取消、追问)
    /agent?id=     某台 agent 的台账 + 按 contextId 串起来的指令流
   鉴权在传输层(HTTPS + 客户端证书)。Task 详情走 SSE, 断线由 EventSource 自动重连补首帧,
   不再维护第二套轮询/旧字段。 */

const PAGE = document.body.dataset.page || 'agents';
const api = (p) => '/api/' + p;

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

/* ---- Task 访问器: 全部只走 A2A 形状 ---- */
const meta = (t) => (t && t.metadata) || {};
const stateOf = (t) => (t && t.status && t.status.state) || S.UNSPECIFIED;
const stateText = (t) => STATE_LABEL[stateOf(t)] || stateOf(t);
const isRunning = (t) => WORKING.indexOf(stateOf(t)) >= 0;
const isTerminal = (t) => !!t && !isRunning(t);
const taskId = (t) => (t && t.id) || '';
const taskAgent = (t) => meta(t).agentId || '';
const taskPrompt = (t) => meta(t).prompt || '';
const taskEvents = (t) => meta(t).events || [];
const taskCost = (t) => meta(t).costMs ? (meta(t).costMs / 1000).toFixed(1) + 's' : (isRunning(t) ? '…' : '-');
const taskRounds = (t) => meta(t).rounds || 0;
function partText(parts) { return (parts || []).map(p => p.text || '').join('\n'); }
function statusMessage(t) {
  const m = t && t.status && t.status.message;
  return m ? partText(m.parts) : '';
}
function artifactText(t) {
  const arts = (t && t.artifacts) || [];
  return arts.map(a => partText(a.parts)).filter(Boolean).join('\n');
}
function badge(t) {
  const st = stateOf(t);
  return `<span class="jz-badge ${BADGE[st] || ''}">${esc(stateText(t))}</span>`;
}

function fixNav() {
  const path = (location.pathname.replace(/\/+$/, '') || '/');
  const key = path.endsWith('/new') ? 'new' : path.endsWith('/tasks') ? 'tasks'
    : path.endsWith('/agent') ? 'agent' : 'agents';
  document.querySelectorAll('#nav a[data-nav]').forEach(a => {
    if (a.dataset.nav === key) a.setAttribute('aria-current', 'page');
    else a.removeAttribute('aria-current');
  });
}

async function getJson(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error('HTTP ' + r.status);
  return r.json();
}
async function postJson(url, body) {
  const r = await fetch(url, {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body || {})
  });
  const j = await r.json().catch(() => ({}));
  return { ok: r.ok, json: j };
}

// 唯一的 SendMessage 入口
async function sendMessage({ agentId, prompt, verifyCmd, contextId }) {
  const body = {
    agentId,
    message: { role: 'ROLE_USER', parts: [{ text: prompt }] },
    returnImmediately: true
  };
  if (verifyCmd) body.verifyCmd = verifyCmd;
  if (contextId) body.contextId = contextId;
  const r = await postJson(api('tasks'), body);
  if (!r.ok) return { ok: false, error: r.json.error || '下发失败' };
  return { ok: true, id: r.json.id, contextId: r.json.contextId };
}

// CancelTask: 202 只是"已请求", 回包是更新后的 Task(状态不一定立刻变)
async function cancelTask(id) {
  const r = await postJson(api('tasks/cancel?id=' + encodeURIComponent(id)), {});
  return r.ok ? { ok: true, task: r.json }
              : { ok: false, error: r.json.error || '取消失败' };
}

/* 条件跟随: 只有"还会变"的进行中数据自动续期, 终态/页面隐藏即停 */
let FOLLOW = 0, FOLLOW_FN = null, FOLLOW_LIVE = false;
function setFollow(live, fn) {
  FOLLOW_LIVE = !!live;
  FOLLOW_FN = fn || null;
  clearTimeout(FOLLOW); FOLLOW = 0;
  if (!FOLLOW_LIVE || !FOLLOW_FN || document.hidden) return;
  FOLLOW = setTimeout(() => { const f = FOLLOW_FN; if (f) f(); }, 1500);
}
document.addEventListener('visibilitychange', () => {
  if (document.hidden) { clearTimeout(FOLLOW); FOLLOW = 0; }
  else setFollow(FOLLOW_LIVE, FOLLOW_FN);
});

/* ============================ /  Agent 台账 ============================ */
function agentCard(a) {
  const online = a.online !== false, builtin = !!a.builtin;
  const href = 'agent?id=' + encodeURIComponent(a.agentId);
  const send = 'new?agentId=' + encodeURIComponent(a.agentId);
  const state = !online ? '<span class="jz-badge">离线</span>'
    : (a.busy ? '<span class="jz-badge jz-badge--info">执行中</span>'
              : '<span class="jz-badge jz-badge--ok">空闲</span>');
  const tail = !online ? `最后在线 ${fmtTime(a.lastSeen)}`
    : (a.taskId ? esc(a.taskId) : (a.lastEventType ? esc(a.lastEventType) : (builtin ? '内置' : '—')));
  const cnt = (a.connects || 0) > 1 ? `<span class="conn__cnt">连过 ${a.connects} 次</span>` : '';
  return `<div class="conn${online ? '' : ' conn--off'}">
    <span class="conn__mono">${esc((a.agentId || '?').slice(0, 2).toUpperCase())}</span>
    <span class="conn__main">
      <span class="conn__id">${esc(a.agentId)} ${builtin ? '<span class="jz-badge">内置</span>' : ''}${cnt}</span>
      <span class="conn__sub">${esc(a.name || '')} · v${esc(a.version || '?')}${
        a.modelName ? ' · ' + esc(a.modelName) : ''} · <code>task</code> / <code>streaming</code></span>
      <span class="conn__path">${esc(a.rootDir || '')}</span>
    </span>
    <span class="conn__side">${state}<span class="conn__task">${tail}</span></span>
    <span class="conn__acts">
      ${online ? `<a class="jz-btn jz-btn--sm jz-btn--primary" href="${send}">下发指令</a>` : ''}
      <a class="jz-btn jz-btn--sm" href="${href}">台账</a>
      ${builtin ? '' : `<button class="jz-btn jz-btn--sm jz-btn--ghost" data-del="${esc(a.agentId)}"
        ${online ? 'disabled title="在线的 agent 不能删除"' : 'title="从台账移除(历史 Task 保留)"'}>删除</button>`}
    </span>
  </div>`;
}

async function loadRunning() {
  const box = document.getElementById('runningList');
  if (!box) return;
  let all = [];
  try { all = (await getJson(api('tasks?limit=50'))).tasks || []; }
  catch (e) { box.innerHTML = `<div class="running__empty">读取失败: ${esc(e)}</div>`; setFollow(false); return; }
  const list = all.filter(isRunning);
  box.innerHTML = list.length ? list.map(t => `
    <div class="running">
      <span class="running__id">${esc(taskId(t))}</span>
      <span class="running__meta">${esc(taskAgent(t))} · ${esc(stateText(t))} · ${esc(meta(t).lastEventType || '-')} · ${taskCost(t)}</span>
      <span class="running__acts">
        <a class="jz-btn jz-btn--sm" href="tasks?id=${encodeURIComponent(taskId(t))}">看 Task</a>
        <button class="jz-btn jz-btn--sm jz-btn--ghost" data-cancel="${esc(taskId(t))}">取消</button>
      </span>
    </div>`).join('')
    : '<div class="running__empty">当前没有在跑的 Task。<a href="new">下发一条指令 →</a></div>';
  box.querySelectorAll('[data-cancel]').forEach(btn => {
    btn.onclick = async () => {
      btn.disabled = true;
      const r = await cancelTask(btn.dataset.cancel);
      if (!r.ok) alert('取消失败：' + r.error);
      loadRunning();
    };
  });
  const kpi = document.getElementById('kpiRunning');
  if (kpi) kpi.textContent = list.length;
  setFollow(list.length > 0, loadRunning);
}

function bindDeletes(box) {
  box.querySelectorAll('[data-del]').forEach(btn => {
    btn.onclick = async (ev) => {
      ev.preventDefault(); ev.stopPropagation();
      const id = btn.dataset.del;
      if (!confirm(`从台账移除 ${id}？\n\n只移除这台 agent 的记录, 它的历史 Task 仍保留。`)) return;
      btn.disabled = true;
      const r = await postJson(api('agent/forget?agentId=' + encodeURIComponent(id)), {});
      if (!r.ok) { alert('删除失败：' + (r.json.error || '失败')); btn.disabled = false; return; }
      loadConnectionsPage();
    };
  });
}

async function loadConnectionsPage() {
  let s;
  try { s = await getJson(api('status')); }
  catch (e) { document.getElementById('meta').textContent = '请求失败: ' + e; return; }
  const hub = s.hub || {}, all = s.known || s.online || [];
  const hubCount = all.filter(a => !a.builtin && a.online !== false).length;
  const offCount = all.filter(a => !a.builtin && a.online === false).length;
  const builtinCount = all.filter(a => a.builtin).length;
  document.getElementById('meta').textContent =
    `控制面 ${hub.listening ? ('监听 ' + hub.port) : '关闭'} · 在线 ${hubCount} 台` +
    (offCount ? ` · 离线 ${offCount}` : '') + (builtinCount ? ` · 内置 ${builtinCount}` : '') +
    ` · 运行 ${((s.uptimeMs || 0) / 1000).toFixed(0)}s`;
  let tasks = [];
  try { tasks = (await getJson(api('tasks?limit=200'))).tasks || []; } catch (e) { /* 台账不依赖历史 */ }
  document.getElementById('cards').innerHTML = [
    ['在线 agent', hubCount, './'],
    ['离线（曾经连过）', offCount, './'],
    ['进行中 Task', tasks.filter(isRunning).length, 'tasks'],
    ['Task 记录', tasks.length, 'tasks']
  ].map(c => `<a class="jz-card kpi" href="${c[2]}"><div class="jz-card__bd">
      <div class="k">${c[0]}</div><div class="v" ${c[0] === '进行中 Task' ? 'id="kpiRunning"' : ''}>${c[1]}</div>
    </div></a>`).join('');
  const box = document.getElementById('connList');
  box.innerHTML = all.length ? all.map(agentCard).join('')
    : `<div class="conn conn--empty">还没有 agent 连过。<br>
       远端: 在 agent_cli 配置里填 <code>server.host/port/token</code>；
       本机: 打开 <code>selfAgent.enabled</code>。</div>`;
  bindDeletes(box);
  loadRunning();
}

/* ============================ /tasks  Task 中心 ============================ */
let ES = null, DETAIL_ID = '', CURRENT_TASK = null;

function closeDetailWatch() {
  if (ES) { ES.close(); ES = null; }
}
function openStream(id) {
  if (typeof EventSource === 'undefined') {
    document.getElementById('detailMsg').textContent = '浏览器不支持 SSE, 请点「刷新」看最新状态';
    return;
  }
  const es = new EventSource(api('tasks/subscribe?id=' + encodeURIComponent(id)));
  ES = es;
  const onAny = () => { if (DETAIL_ID === id) loadDetail(id, true).catch(() => {}); };
  ['task', 'status_update', 'artifact_update'].forEach(n => es.addEventListener(n, onAny));
  // EventSource 会自动重连; 重连首帧是完整 Task 快照, 所以不需要第二套轮询
  es.onerror = () => {
    const m = document.getElementById('detailMsg');
    if (m) m.textContent = 'SSE 断开, 浏览器正在自动重连…';
  };
}

function taskRows(list) {
  if (!list.length) return `<tr><td colspan="7" style="color:var(--jz-muted)">没有符合条件的 Task</td></tr>`;
  return list.map(t => `<tr data-task="${esc(taskId(t))}" style="cursor:pointer">
      <td data-label="Task">${esc(taskId(t))}</td>
      <td data-label="agent">${esc(taskAgent(t))}</td>
      <td data-label="状态">${badge(t)}</td>
      <td data-label="最近事件" class="hide-sm">${esc(meta(t).lastEventType || '-')}</td>
      <td data-label="耗时" class="hide-sm">${taskCost(t)}</td>
      <td data-label="提交时间" class="hide-sm">${fmtTime(meta(t).startMs)}</td>
      <td data-label="指令" style="color:var(--jz-muted);font-size:var(--jz-fs-1)">${esc(taskPrompt(t).slice(0, 60))}</td>
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
      <div class="msg__meta">${esc(m.role === 'ROLE_USER' ? '下发指令' : 'agent')} · ${esc(m.messageId || '')}</div>
    </div>`).join('');
}

function renderDetail(t) {
  CURRENT_TASK = t;
  document.getElementById('detail').classList.remove('hidden');
  document.getElementById('detailHead').innerHTML = `<div class="task-head">
      <div><span class="task-head__id">${esc(taskId(t))}</span> ${badge(t)}</div>
      <div class="muted">${esc(taskAgent(t))} · ${taskCost(t)} · ${taskRounds(t)} 轮 · ${fmtTime(meta(t).startMs)} → ${fmtTime(meta(t).endMs)}</div>
      <div class="muted">contextId: <code>${esc(t.contextId || '-')}</code>${t.status && t.status.timestamp ? ' · ' + esc(t.status.timestamp) : ''}</div>
    </div>`;

  const followUp = 'new?agentId=' + encodeURIComponent(taskAgent(t)) +
                   '&contextId=' + encodeURIComponent(t.contextId || '');
  const acts = document.getElementById('detailActs');
  acts.innerHTML = `
    ${isRunning(t) ? `<button class="jz-btn jz-btn--sm jz-btn--ghost" id="btnCancelTask">取消 Task</button>` : ''}
    <a class="jz-btn jz-btn--sm" href="${followUp}">继续追问（同 contextId）</a>
    <a class="jz-btn jz-btn--sm" href="agent?id=${encodeURIComponent(taskAgent(t))}">该 agent 台账</a>
    <button class="jz-btn jz-btn--sm" id="btnRefreshDetail">刷新</button>
    <button class="jz-btn jz-btn--sm jz-btn--ghost" id="btnCloseDetail">关闭</button>
    <span class="logbar__hint" id="detailMsg"></span>`;
  const cancelBtn = document.getElementById('btnCancelTask');
  if (cancelBtn) cancelBtn.onclick = async () => {
    cancelBtn.disabled = true;
    const r = await cancelTask(taskId(t));
    document.getElementById('detailMsg').textContent = r.ok
      ? '已请求取消（A2A: 不保证成功），继续跟随 Task 状态直到终态'
      : '取消失败：' + r.error;
    loadDetail(taskId(t), true);
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
    <div class="kv"><span class="k">agent 回复</span><span class="v"><pre class="jz-log" style="max-height:220px">${esc(artifactText(t))}</pre></span></div>`;
}

async function loadDetail(id, fromStream) {
  if (DETAIL_ID !== id) { closeDetailWatch(); DETAIL_ID = id; CURRENT_TASK = null; }
  let t;
  try { t = await getJson(api('tasks/detail?id=' + encodeURIComponent(id))); }
  catch (e) {
    document.getElementById('detail').classList.remove('hidden');
    document.getElementById('detailBody').innerHTML = `<span style="color:var(--jz-fail)">读取失败: ${esc(e)}</span>`;
    return;
  }
  renderDetail(t);
  if (isTerminal(t)) {
    closeDetailWatch();
    if (fromStream) refreshListOnly();
  } else if (!fromStream && !ES) {
    openStream(id);
  }
}
function closeDetail() {
  closeDetailWatch();
  DETAIL_ID = ''; CURRENT_TASK = null;
  document.getElementById('detail').classList.add('hidden');
  history.replaceState(null, '', 'tasks');
}
async function refreshListOnly() {
  try {
    const data = await getJson(api(listQuery()));
    document.getElementById('listBody').innerHTML = taskRows(data.tasks || []);
    bindTaskRows();
  } catch (e) { /* 列表刷新失败不影响详情 */ }
}
function listQuery() {
  const agent = document.getElementById('fAgent').value;
  const state = document.getElementById('fState').value;
  const contextId = document.getElementById('fContext').value.trim();
  const limit = document.getElementById('fLimit').value || 50;
  return 'tasks?limit=' + encodeURIComponent(limit) +
    (agent ? '&agentId=' + encodeURIComponent(agent) : '') +
    (state ? '&state=' + encodeURIComponent(state) : '') +
    (contextId ? '&contextId=' + encodeURIComponent(contextId) : '');
}
function bindTaskRows() {
  document.querySelectorAll('#listBody tr[data-task]').forEach(tr => {
    tr.onclick = () => {
      const id = tr.dataset.task;
      history.replaceState(null, '', 'tasks?id=' + encodeURIComponent(id));
      loadDetail(id);
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
  document.getElementById('meta').textContent = `共 ${data.totalSize || 0} 条记录，当前显示 ${list.length} 条`;
  document.getElementById('hint').textContent = '点一行看 Task 详情';
  document.getElementById('listBody').innerHTML = taskRows(list);
  bindTaskRows();
  const id = url.searchParams.get('id');
  if (id) loadDetail(id);
}

/* ============================ /new  SendMessage ============================ */
async function loadNewPage() {
  const sel = document.getElementById('fAgent');
  const params = new URLSearchParams(location.search);
  const presetAgent = params.get('agentId') || '', presetCtx = params.get('contextId') || '';
  if (presetCtx) document.getElementById('fContext').value = presetCtx;
  try {
    const s = await getJson(api('status'));
    const ids = (s.known || s.online || []).filter(a => a.online !== false).map(a => a.agentId);
    sel.innerHTML = ids.length ? ids.map(id => `<option value="${esc(id)}">${esc(id)}</option>`).join('')
                               : '<option value="">(没有在线 agent)</option>';
    if (presetAgent && ids.indexOf(presetAgent) >= 0) sel.value = presetAgent;
  } catch (e) { sel.innerHTML = '<option value="">(读取失败)</option>'; }
  document.getElementById('btnReset').onclick = () => {
    document.getElementById('fPrompt').value = '';
    document.getElementById('fVerify').value = '';
    document.getElementById('fContext').value = presetCtx;
    document.getElementById('msg').textContent = '';
  };
  document.getElementById('btnSubmit').onclick = async () => {
    const msg = document.getElementById('msg'), btn = document.getElementById('btnSubmit');
    const prompt = document.getElementById('fPrompt').value.trim();
    if (!sel.value) { msg.textContent = '没有在线的 agent'; return; }
    if (!prompt) { msg.textContent = '指令不能为空'; return; }
    btn.disabled = true; msg.textContent = 'SendMessage …';
    const r = await sendMessage({
      agentId: sel.value, prompt,
      verifyCmd: document.getElementById('fVerify').value.trim(),
      contextId: document.getElementById('fContext').value.trim()
    });
    if (!r.ok) { msg.textContent = '失败：' + r.error; btn.disabled = false; return; }
    msg.textContent = '已创建 Task ' + r.id;
    location.href = 'tasks?id=' + encodeURIComponent(r.id);
  };
}

/* ============================ /agent?id=  台账 ============================ */
let AGENT_ID = '', AGENT_OFFLINE = false, AGENT_CTX = '', FEED_SIG = '', SUBMIT_BOUND = false;
const TABS = ['chat', 'tasks', 'card'];
function curTab() { const t = (location.hash || '#chat').slice(1); return TABS.includes(t) ? t : 'chat'; }
function setTab(name) {
  document.querySelectorAll('#tabs .tab').forEach(a => a.classList.toggle('is-on', a.dataset.tab === name));
  document.querySelectorAll('.pane').forEach(p => p.classList.toggle('hidden', p.dataset.pane !== name));
}
function scrollFeed(force) {
  const f = document.getElementById('feed');
  if (!f) return;
  if (force || f.scrollHeight - f.scrollTop - f.clientHeight < 80) f.scrollTop = f.scrollHeight;
}
function cardHtml(card) {
  if (!card) return '<span class="muted">没有 AgentCard</span>';
  const m = card.metadata || {};
  const skills = (card.skills || []).map(s => `<span class="skill">${esc(s.name || s.id)}
    <span class="muted">${esc((s.tags || []).join('/'))}</span></span>`).join('');
  const caps = Object.keys(card.capabilities || {}).filter(k => card.capabilities[k]).join(' · ') || '-';
  return `
    <div class="kv"><span class="k">名称</span><span class="v">${esc(card.name)} <span class="muted">v${esc(card.version)}</span></span></div>
    <div class="kv"><span class="k">说明</span><span class="v">${esc(card.description || '-')}</span></div>
    <div class="kv"><span class="k">接口</span><span class="v"><code>${esc((card.supportedInterfaces || [{}])[0].url || '-')}</code>
      <span class="muted">${esc((card.supportedInterfaces || [{}])[0].protocolBinding || '')}</span></span></div>
    <div class="kv"><span class="k">能力</span><span class="v">${esc(caps)}</span></div>
    <div class="kv"><span class="k">安全</span><span class="v">${esc(Object.keys(card.securitySchemes || {}).join(', ') || '-')}</span></div>
    <div class="kv"><span class="k">skills</span><span class="v"><div class="skills">${skills || '<span class="muted">-</span>'}</div></span></div>
    <div class="kv"><span class="k">工作区</span><span class="v">${esc(m.rootDir || '-')}</span></div>
    <div class="kv"><span class="k">模型</span><span class="v">${esc(m.modelName || '-')}</span></div>
    <div class="kv"><span class="k">状态</span><span class="v">${m.online
      ? (m.busy ? '<span class="jz-badge jz-badge--info">执行中</span>' : '<span class="jz-badge jz-badge--ok">在线</span>')
      : '<span class="jz-badge">离线</span>'} ${m.taskId ? '<code>' + esc(m.taskId) + '</code>' : ''}</span></div>
    <div class="kv"><span class="k">首次 / 末次</span><span class="v">${fmtTime(m.firstSeen)} → ${fmtTime(m.lastSeen)}</span></div>`;
}
function feedHtml(tasks) {
  if (!tasks.length) return `<div class="feed__empty">还没有下发给这台 agent 的指令。</div>`;
  return tasks.slice().reverse().map(t => {
    const body = isRunning(t)
      ? `<span class="muted">${esc(stateText(t))}…${meta(t).lastEventType ? ' <code>' + esc(meta(t).lastEventType) + '</code>' : ''}</span>`
      : esc(artifactText(t) || statusMessage(t) || '（无输出）');
    return `<div class="msg msg--user">
        <div class="bubble">${esc(taskPrompt(t))}</div>
        <div class="msg__meta">${fmtTime(meta(t).startMs)} · <a href="tasks?id=${encodeURIComponent(taskId(t))}">${esc(taskId(t))}</a></div>
      </div>
      <div class="msg msg--agent" data-task="${esc(taskId(t))}" style="cursor:pointer" title="打开 Task 详情">
        <div class="bubble">${body}</div>
        <div class="msg__meta">${badge(t)} ${taskRounds(t)} 轮 · ${taskCost(t)} · <code>${esc(t.contextId || '')}</code></div>
      </div>`;
  }).join('');
}
function renderFeed(tasks) {
  const sig = tasks.map(t => `${taskId(t)}:${stateOf(t)}:${taskEvents(t).length}:${meta(t).lastEventType || ''}`).join('|');
  if (sig === FEED_SIG) return;
  FEED_SIG = sig;
  document.getElementById('feed').innerHTML = feedHtml(tasks);
  document.querySelectorAll('#feed [data-task]').forEach(el => {
    el.onclick = () => { location.href = 'tasks?id=' + encodeURIComponent(el.dataset.task); };
  });
  scrollFeed(!renderFeed.inited);
  renderFeed.inited = true;
}
function renderTaskTable(tasks) {
  document.getElementById('taskBody').innerHTML = tasks.length ? tasks.map(t => `<tr style="cursor:pointer"
      onclick="location.href='tasks?id=${encodeURIComponent(taskId(t))}'">
      <td data-label="Task">${esc(taskId(t))}</td><td data-label="状态">${badge(t)}</td>
      <td data-label="轮次" class="hide-sm">${taskRounds(t)}</td>
      <td data-label="耗时" class="hide-sm">${taskCost(t)}</td>
      <td data-label="提交时间" class="hide-sm">${fmtTime(meta(t).startMs)}</td></tr>`).join('')
    : `<tr><td colspan="5" style="color:var(--jz-muted)">这台 agent 还没有 Task</td></tr>`;
}
function renderNodes(card) {
  const nodes = (card && card.metadata && card.metadata.nodes) || [];
  document.getElementById('nodeBody').innerHTML = nodes.length ? nodes.map(n => `<tr>
      <td data-label="节点">${esc(n.name || '-')}</td>
      <td data-label="地址">${esc(n.target || '-')}</td>
      <td data-label="状态">${n.online ? '<span class="jz-badge jz-badge--ok">在线</span>' : '<span class="jz-badge">离线</span>'}</td>
      <td data-label="工作区" class="muted">${esc(n.rootDir || '-')}</td></tr>`).join('')
    : `<tr><td colspan="4" style="color:var(--jz-muted)">没有节点（本地执行）</td></tr>`;
}
function bindComposer() {
  if (SUBMIT_BOUND) return;
  SUBMIT_BOUND = true;
  document.getElementById('btnSubmit').onclick = doSubmit;
  document.getElementById('fPrompt').addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) { e.preventDefault(); doSubmit(); }
  });
}
async function doSubmit() {
  const msg = document.getElementById('msg'), btn = document.getElementById('btnSubmit');
  const prompt = document.getElementById('fPrompt').value.trim();
  if (AGENT_OFFLINE) { msg.textContent = '这台 agent 已离线，去「下发指令」页选在线机器'; return; }
  if (!prompt) { msg.textContent = '指令不能为空'; return; }
  btn.disabled = true; msg.textContent = 'SendMessage …';
  const r = await sendMessage({ agentId: AGENT_ID, prompt,
    verifyCmd: document.getElementById('fVerify').value.trim(), contextId: AGENT_CTX });
  if (!r.ok) { msg.textContent = '失败：' + r.error; btn.disabled = false; return; }
  msg.innerHTML = '已创建 <a href="tasks?id=' + encodeURIComponent(r.id) + '">Task ' + esc(r.id) + '</a>';
  document.getElementById('fPrompt').value = '';
  FEED_SIG = ''; btn.disabled = false;
  loadAgentPage();
}
async function loadAgentPage() {
  const id = new URLSearchParams(location.search).get('id') || '';
  if (!id) { document.getElementById('meta').textContent = '缺少 ?id='; return; }
  if (AGENT_ID !== id) {
    AGENT_ID = id; FEED_SIG = '';
    AGENT_CTX = new URLSearchParams(location.search).get('contextId') || ('ctx-' + id + '-' + Date.now());
    document.getElementById('title').textContent = id;
    document.getElementById('mono').textContent = id.slice(0, 2).toUpperCase();
    document.getElementById('allTasks').href = 'tasks?agentId=' + encodeURIComponent(id);
    const newLink = document.querySelector('#nav a[data-nav="new"]');
    if (newLink) newLink.href = 'new?agentId=' + encodeURIComponent(id) + '&contextId=' + encodeURIComponent(AGENT_CTX);
    history.replaceState(null, '', 'agent?id=' + encodeURIComponent(id) + '&contextId=' + encodeURIComponent(AGENT_CTX));
    bindComposer();
  }
  let d;
  try { d = await getJson(api('agent?id=' + encodeURIComponent(id))); }
  catch (e) {
    document.getElementById('meta').textContent = '这台 agent 查不到（可能已被删除）';
    document.getElementById('feed').innerHTML = `<div class="feed__empty">server 上没有这台 agent 的记录。<br>
      它的历史 Task 还在：<a href="tasks?agentId=${encodeURIComponent(id)}">Task 记录</a></div>`;
    AGENT_OFFLINE = true; disableComposer(); return;
  }
  const info = d.info || {}, kind = d.kind || '', card = d.card || null;
  AGENT_OFFLINE = kind === 'offline' || info.online === false;
  document.getElementById('meta').textContent = AGENT_OFFLINE
    ? `已离线 · 最后在线 ${fmtTime(info.lastSeen)} · 只能看历史`
    : `${kind === 'builtin' ? '内置（server 进程内）' : (kind === 'process' ? '本地子进程' : '远端 cli')} · ${info.busy ? '执行中' : '空闲'} · contextId <code>${esc(AGENT_CTX)}</code>`;
  document.getElementById('ctxLine').textContent = '同一 contextId 的指令串成一段会话：' + AGENT_CTX;
  document.getElementById('infoBody').innerHTML = cardHtml(card);
  renderNodes(card);
  const tasks = d.tasks || [];
  renderTaskTable(tasks); renderFeed(tasks);
  disableComposer(AGENT_OFFLINE || kind === 'process');
  if ((AGENT_OFFLINE || kind === 'process') && !document.getElementById('msg').textContent)
    document.getElementById('msg').textContent = AGENT_OFFLINE ? '这台 agent 已离线，去「下发指令」页选在线机器' : '';
  setFollow(tasks.some(isRunning), loadAgentPage);
}
function disableComposer(on) {
  if (on === undefined) on = true;
  document.getElementById('btnSubmit').disabled = on;
  document.getElementById('fPrompt').disabled = on;
  document.getElementById('fVerify').disabled = on;
}

/* ============================ 启动 ============================ */
fixNav();
if (PAGE === 'agents') {
  loadConnectionsPage();
  document.getElementById('btnRefresh').onclick = loadConnectionsPage;
} else if (PAGE === 'agent') {
  setTab(curTab());
  window.addEventListener('hashchange', () => setTab(curTab()));
  loadAgentPage();
  document.getElementById('btnRefresh').onclick = () => { FEED_SIG = ''; loadAgentPage(); };
  setTimeout(() => {
    const f = document.getElementById('feed');
    if (f && f.textContent.indexOf('加载中') >= 0)
      f.innerHTML = `<div class="feed__empty">4 秒还没拿到数据。按 <code>Ctrl+F5</code> 强制刷新试试。</div>`;
  }, 4000);
} else if (PAGE === 'tasks') {
  loadTasksPage();
  document.getElementById('btnReload').onclick = loadTasksPage;
  ['fAgent', 'fState', 'fLimit', 'fContext'].forEach(key => {
    const el = document.getElementById(key);
    if (el) el.onchange = loadTasksPage;
  });
  document.getElementById('btnClose').onclick = closeDetail;
} else if (PAGE === 'new') {
  loadNewPage();
}
