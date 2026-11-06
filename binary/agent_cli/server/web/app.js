/* JZAgent 管理台: 连接式多页
     /              连接列表(每个 agent 一张卡片, 点「进入」进详情)
     /agent?id=xxx  详情: 会话(输入+结果) / 任务 / 信息 三个 tab
     /tasks         任务记录(可筛选) + ?id= 详情
     /new           新建任务(跨 agent)
   数据: /api/status(含 known, 离线也在列表里), /api/agent, /api/tasks。 */

const TOKEN = new URLSearchParams(location.search).get('token') || '';
const PAGE = document.body.dataset.page || 'agents';
const withToken = (p) => p + (TOKEN ? ((p.includes('?') ? '&' : '?') + 'token=' + encodeURIComponent(TOKEN)) : '');
const api = (p) => withToken('/api/' + p);

// 页面级兜底: 有 #msg 的地方(会话页)直接把异常显示出来, 否则进 console。
// 没有这个, 脚本一早抛错就只表现为"界面一直显示加载中", 从截图/截图式反馈根本看不出来。
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

function fmtCost(t) {
  if (t && t.cost_ms) return (t.cost_ms / 1000).toFixed(1) + 's';
  return t && t.status === 'running' ? '…' : '-';
}

const BADGE = {
  running: 'jz-badge--info', success: 'jz-badge--ok', failed: 'jz-badge--fail',
  cancelled: 'jz-badge--warn', lost: 'jz-badge--warn'
};
const badge = (s) => `<span class="jz-badge ${BADGE[s] || ''}">${esc(s || '?')}</span>`;

function fixNav() {
  document.querySelectorAll('#nav a').forEach(a => {
    a.href = withToken(a.getAttribute('href'));
    if (a.dataset.nav === PAGE) a.setAttribute('aria-current', 'page');
  });
}

async function getJson(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error('HTTP ' + r.status);
  return r.json();
}

/* ============================ 连接列表 ============================ */
function connCard(a) {
  const nodes = a.nodes || [];
  const on = nodes.filter(n => n.online).length;
  const builtin = !!a.builtin;
  const online = a.online !== false;
  const href = withToken('agent?id=' + encodeURIComponent(a.agentId));

  const state = !online
    ? '<span class="jz-badge">离线</span>'
    : (a.busy ? '<span class="jz-badge jz-badge--info">跑任务</span>'
              : '<span class="jz-badge jz-badge--ok">空闲</span>');
  const tail = !online
    ? `最后在线 ${fmtTime(a.lastSeen)}`
    : (a.taskId ? esc(a.taskId)
                : (a.lastEventType ? esc(a.lastEventType) : (builtin ? '内置' : '—')));
  const cnt = (a.connects || 0) > 1 ? `<span class="conn__cnt">连过 ${a.connects} 次</span>` : '';

  return `<div class="conn${online ? '' : ' conn--off'}" data-id="${esc(a.agentId)}">
    <span class="conn__mono">${esc((a.agentId || '?').slice(0, 2).toUpperCase())}</span>
    <span class="conn__main">
      <span class="conn__id">${esc(a.agentId)} ${builtin ? '<span class="jz-badge">内置</span>' : ''}${cnt}</span>
      <span class="conn__sub">${esc(a.name || '')} · v${esc(a.version || '?')}${
        a.modelName ? ' · ' + esc(a.modelName) : ''
      }${nodes.length ? ' · 节点 ' + on + '/' + nodes.length : ''}</span>
      <span class="conn__path">${esc(a.rootDir || '')}</span>
    </span>
    <span class="conn__side">
      ${state}
      <span class="conn__task">${tail}</span>
    </span>
    <span class="conn__acts">
      <a class="jz-btn jz-btn--sm jz-btn--primary" href="${href}">进入</a>
      ${builtin ? '' : `<button class="jz-btn jz-btn--sm jz-btn--ghost"
        data-del="${esc(a.agentId)}" title="从列表删除(历史任务保留)">删除</button>`}
    </span>
  </div>`;
}

// 删除 = 只从"曾经连过"的列表里移除; 在线时后端会拒绝(409)
function bindDeletes(box) {
  box.querySelectorAll('[data-del]').forEach(btn => {
    btn.onclick = async (ev) => {
      ev.preventDefault();
      ev.stopPropagation();
      const id = btn.dataset.del;
      if (!confirm(`从列表删除 ${id}？\n\n只会移除这台 agent 的记录，它的历史任务仍保留在「任务记录」里。`))
        return;
      btn.disabled = true;
      try {
        const r = await fetch(api('agent/forget?agentId=' + encodeURIComponent(id)), { method: 'POST' });
        const j = await r.json().catch(() => ({}));
        if (!r.ok) { alert('删除失败：' + (j.error || ('HTTP ' + r.status))); btn.disabled = false; return; }
        loadConnectionsPage();
      } catch (e) {
        alert('删除失败：' + e);
        btn.disabled = false;
      }
    };
  });
}

async function loadConnectionsPage() {
  let s;
  try {
    s = await getJson(api('status'));
  } catch (e) {
    document.getElementById('meta').textContent = '请求失败: ' + e;
    return;
  }

  const hub = s.hub || {};
  // known = 曾经连过的全部(含离线); 老服务端没有这个字段时退回 online
  const all = s.known || s.online || [];
  const hubCount = all.filter(a => !a.builtin && a.online !== false).length;
  const offCount = all.filter(a => !a.builtin && a.online === false).length;
  const builtinCount = all.filter(a => a.builtin).length;

  document.getElementById('meta').textContent =
    `控制面 ${hub.listening ? ('监听 ' + hub.port) : '关闭'} · 在线 ${hubCount} 台` +
    (offCount ? ` · 离线 ${offCount}` : '') +
    (builtinCount ? ` · 内置 ${builtinCount}` : '') +
    ` · 运行 ${((s.uptimeMs || 0) / 1000).toFixed(0)}s`;

  let tasks = [];
  try {
    tasks = (await getJson(api('tasks?limit=200'))).tasks || [];
  } catch (e) { /* 记录读不到不影响连接列表 */ }

  document.getElementById('cards').innerHTML = [
    ['在线 agent', hubCount],
    ['离线（曾经连过）', offCount],
    ['跑任务中', tasks.filter(t => t.status === 'running').length],
    ['任务记录', tasks.length],
  ].map(c => `<div class="jz-card kpi"><div class="jz-card__bd">` +
             `<div class="k">${c[0]}</div><div class="v">${c[1]}</div></div></div>`).join('');

  const box = document.getElementById('connList');
  box.innerHTML = all.length
    ? all.map(connCard).join('')
    : `<div class="conn conn--empty">还没有 agent 连过。<br>
       远端: 在 agent_cli 配置里填 <code>server.host/port/token</code>；
       本机: 打开 <code>selfAgent.enabled</code>。</div>`;
  bindDeletes(box);
}

/* ============================ agent 详情 ============================ */
let AGENT_ID = '';
let AGENT_OFFLINE = false;
let FEED_SIG = '';          // 内容没变就不重绘(否则每次轮询都会把滚动位置打回顶部)
let SUBMIT_BOUND = false;
let AGENT_POLL = 0;         // 只在"有任务在跑"时自我续期一次, 不是常驻定时器

const TABS = ['chat', 'tasks', 'info'];

function curTab() {
  const t = (location.hash || '#chat').slice(1);
  return TABS.includes(t) ? t : 'chat';
}

function setTab(name) {
  document.querySelectorAll('#tabs .tab').forEach(a => a.classList.toggle('is-on', a.dataset.tab === name));
  document.querySelectorAll('.pane').forEach(p => p.classList.toggle('hidden', p.dataset.pane !== name));
  if (name === 'chat') scrollFeed(true);
}

function scrollFeed(force) {
  const f = document.getElementById('feed');
  if (!f) return;
  const nearBottom = f.scrollHeight - f.scrollTop - f.clientHeight < 80;
  if (force || nearBottom) f.scrollTop = f.scrollHeight;
}

// 会话: 一条任务 = 一个"你说了什么" + 一个"它回了什么"
function feedHtml(tasks) {
  if (!tasks.length)
    return `<div class="feed__empty">还没有任务。在下面输入第一句。</div>`;

  return tasks.slice().reverse().map(t => {   // 接口是"新的在前", 会话要"旧的在上"
    const body = t.status === 'running'
      ? `<span class="muted">执行中…${t.last_event ? ' <code>' + esc(t.last_event) + '</code>' : ''}</span>`
      : (t.agent_reply
          ? esc(t.agent_reply)
          : (t.error
              ? `<span style="color:var(--jz-fail)">${esc(t.error)}</span>`
              : '<span class="muted">（无输出）</span>'));

    return `<div class="msg msg--user">
        <div class="bubble">${esc(t.prompt)}</div>
        <div class="msg__meta">${fmtTime(t.start_ms)} · ${esc(t.taskId)}</div>
      </div>
      <div class="msg msg--agent">
        <div class="bubble">${body}</div>
        <div class="msg__meta">${badge(t.status)} ${t.verify_rounds || 0} 轮 · ${fmtCost(t)}</div>
      </div>`;
  }).join('');
}

let FEED_INITED = false;

function renderFeed(tasks) {
  const sig = tasks.map(t => `${t.taskId}:${t.status}:${(t.agent_reply || '').length}:${t.last_event || ''}`).join('|');
  if (sig === FEED_SIG) return;
  FEED_SIG = sig;

  // 首次渲染要直接定位到最新一条(会话习惯), 之后只有原本就在底部才自动跟
  const first = !FEED_INITED;
  FEED_INITED = true;

  document.getElementById('feed').innerHTML = feedHtml(tasks);
  scrollFeed(first);
}

function renderInfo(info, kind) {
  const offline = kind === 'offline' || info.online === false;
  document.getElementById('infoBody').innerHTML = `
    <div class="kv"><span class="k">agentId</span><span class="v">${esc(info.agentId || AGENT_ID)}</span></div>
    <div class="kv"><span class="k">名称</span><span class="v">${esc(info.name || '-')} ${esc(info.version || '')}</span></div>
    <div class="kv"><span class="k">模型</span><span class="v">${esc(info.modelName || '-')}</span></div>
    <div class="kv"><span class="k">工作区</span><span class="v">${esc(info.rootDir || '-')}</span></div>
    <div class="kv"><span class="k">状态</span><span class="v">${
      info.busy ? badge('running') : (offline ? '<span class="jz-badge">离线</span>'
                                              : '<span class="jz-badge jz-badge--ok">在线</span>')
    }</span></div>
    <div class="kv"><span class="k">首次 / 末次</span><span class="v">${fmtTime(info.firstSeen)} → ${fmtTime(info.lastSeen)}${
      info.connects ? `　（连过 ${info.connects} 次）` : ''
    }</span></div>`;
}

function renderNodes(info) {
  const nodes = info.nodes || [];
  document.getElementById('nodeBody').innerHTML = nodes.length ? nodes.map(n => `<tr>
      <td data-label="节点">${esc(n.name || '-')}</td>
      <td data-label="地址">${esc(n.host || '-')}</td>
      <td data-label="状态">${n.online ? '<span class="jz-badge jz-badge--ok">在线</span>'
                                       : '<span class="jz-badge">离线</span>'}</td>
      <td data-label="工作区" style="color:var(--jz-muted);font-size:var(--jz-fs-1)">${esc(n.rootDir || '-')}</td>
    </tr>`).join('')
    : `<tr><td colspan="4" style="color:var(--jz-muted)">没有节点（本地执行）</td></tr>`;
}

function renderTaskTable(tasks) {
  document.getElementById('taskBody').innerHTML = tasks.length ? tasks.map(t => `<tr style="cursor:pointer"
      onclick="location.href='${withToken('tasks?id=' + encodeURIComponent(t.taskId))}'">
      <td data-label="任务">${esc(t.taskId)}</td>
      <td data-label="状态">${badge(t.status)}</td>
      <td data-label="轮次" class="hide-sm">${t.verify_rounds || 0}</td>
      <td data-label="耗时" class="hide-sm">${fmtCost(t)}</td>
      <td data-label="提交时间" class="hide-sm">${fmtTime(t.start_ms)}</td>
    </tr>`).join('')
    : `<tr><td colspan="5" style="color:var(--jz-muted)">这台 agent 还没有任务</td></tr>`;
}

function bindComposer() {
  if (SUBMIT_BOUND) return;
  SUBMIT_BOUND = true;

  const btn = document.getElementById('btnSubmit');
  const ta = document.getElementById('fPrompt');
  btn.onclick = doSubmit;
  ta.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) { e.preventDefault(); doSubmit(); }
  });
}

async function doSubmit() {
  const msg = document.getElementById('msg');
  const btn = document.getElementById('btnSubmit');
  const prompt = document.getElementById('fPrompt').value.trim();
  const verify = document.getElementById('fVerify').value.trim();

  if (AGENT_OFFLINE) { msg.textContent = '这台 agent 已离线，无法派活'; return; }
  if (!prompt) { msg.textContent = '任务不能为空'; return; }

  btn.disabled = true;
  msg.textContent = '提交中…';
  try {
    const body = { agentId: AGENT_ID, prompt, use_memory: false, wait: false };
    if (verify) body.verify_cmd = verify;
    const r = await fetch(api('tasks'), {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    });
    const j = await r.json().catch(() => ({}));
    if (!r.ok) { msg.textContent = '失败(' + r.status + ')：' + (j.error || ''); return; }
    msg.textContent = '已提交 ' + (j.taskId || '');
    document.getElementById('fPrompt').value = '';
    FEED_SIG = '';              // 强制重绘, 让新任务立刻出现在会话里
    loadAgentPage();
    scrollFeed(true);
  } catch (e) {
    msg.textContent = '请求失败：' + e;
  } finally {
    btn.disabled = false;
  }
}

async function loadAgentPage() {
  const id = new URLSearchParams(location.search).get('id') || '';
  if (!id) {
    document.getElementById('meta').textContent = '缺少 ?id=';
    document.getElementById('feed').innerHTML = `<div class="feed__empty">缺少 ?id=</div>`;
    return;
  }

  if (AGENT_ID !== id) {
    AGENT_ID = id;
    FEED_SIG = '';
    document.getElementById('title').textContent = id;
    document.getElementById('mono').textContent = id.slice(0, 2).toUpperCase();
    document.getElementById('allTasks').href = withToken('tasks?agentId=' + encodeURIComponent(id));
    bindComposer();
  }

  let d;
  try {
    d = await getJson(api('agent?id=' + encodeURIComponent(id)));
  } catch (e) {
    document.getElementById('meta').textContent = '这台 agent 查不到（可能已被删除）';
    document.getElementById('feed').innerHTML =
      `<div class="feed__empty">server 上没有这台 agent 的记录。<br>
       它的历史任务还在：<a href="${withToken('tasks?agentId=' + encodeURIComponent(id))}">任务记录</a></div>`;
    document.getElementById('btnSubmit').disabled = true;
    AGENT_OFFLINE = true;
    return;
  }

  const info = d.info || {};
  const kind = d.kind || '';
  const builtin = kind === 'builtin';
  const offline = kind === 'offline' || info.online === false;
  AGENT_OFFLINE = offline;

  document.getElementById('builtinNote').style.display = builtin ? '' : 'none';
  const stateText = builtin ? '内置（server 进程内）'
    : (kind === 'process' ? '本地子进程' : (offline ? '已离线' : '远端 cli（控制面连接）'));
  document.getElementById('meta').textContent = offline
    ? `${stateText} · 最后在线 ${fmtTime(info.lastSeen)}`
    : `${stateText} · ${info.busy ? '跑任务中' : '空闲'}` + (info.lastSeen ? ` · 心跳 ${fmtTime(info.lastSeen)}` : '');

  renderInfo(info, kind);
  renderNodes(info);
  const tasks = d.tasks || [];
  renderTaskTable(tasks);
  renderFeed(tasks);

  // 没有常驻定时刷新: 只有"确实有任务在跑"时才追一次, 跑完自然停
  clearTimeout(AGENT_POLL);
  if (tasks.some(t => t.status === 'running'))
    AGENT_POLL = setTimeout(loadAgentPage, 1500);

  // 离线 / 本地子进程: 派不了活, 直接把输入区关掉
  const disabled = offline || kind === 'process';
  document.getElementById('btnSubmit').disabled = disabled;
  document.getElementById('fPrompt').disabled = disabled;
  document.getElementById('fVerify').disabled = disabled;
  if (disabled && !document.getElementById('msg').textContent)
    document.getElementById('msg').textContent = offline ? '这台 agent 已离线，无法派活' : '';
}

/* ============================ tasks 页 ============================ */
function taskRows(list) {
  if (!list.length) {
    return `<tr><td colspan="7" style="color:var(--jz-muted)">没有符合条件的任务</td></tr>`;
  }
  return list.map(t => `<tr data-task="${esc(t.taskId)}" style="cursor:pointer">
      <td data-label="任务">${esc(t.taskId)}</td>
      <td data-label="agent">${esc(t.agentId)}</td>
      <td data-label="状态">${badge(t.status)}</td>
      <td data-label="轮次" class="hide-sm">${t.verify_rounds || 0}</td>
      <td data-label="耗时" class="hide-sm">${fmtCost(t)}</td>
      <td data-label="提交时间" class="hide-sm">${fmtTime(t.start_ms)}</td>
      <td data-label="任务内容" style="color:var(--jz-muted);font-size:var(--jz-fs-1)">${esc((t.prompt || '').slice(0, 60))}</td>
    </tr>`).join('');
}

async function loadDetail(id) {
  const box = document.getElementById('detail');
  const body = document.getElementById('detailBody');
  box.classList.remove('hidden');
  body.innerHTML = '<span style="color:var(--jz-muted)">加载中…</span>';
  try {
    const t = await getJson(api('tasks/detail?id=' + encodeURIComponent(id)));
    body.innerHTML = `
      <div class="kv"><span class="k">taskId</span><span class="v">${esc(t.taskId)}</span></div>
      <div class="kv"><span class="k">agent</span><span class="v">
        <a href="${withToken('agent?id=' + encodeURIComponent(t.agentId))}">${esc(t.agentId)}</a></span></div>
      <div class="kv"><span class="k">状态</span><span class="v">${badge(t.status)} ${esc(t.error || '')}</span></div>
      <div class="kv"><span class="k">轮次 / 耗时</span><span class="v">${t.verify_rounds || 0} 轮 · ${fmtCost(t)}</span></div>
      <div class="kv"><span class="k">提交 / 结束</span><span class="v">${fmtTime(t.start_ms)} → ${fmtTime(t.end_ms)}</span></div>
      <div class="kv"><span class="k">最近事件</span><span class="v">${esc(t.last_event || '-')}</span></div>
      <div class="kv"><span class="k">验证命令</span><span class="v">${t.verify_cmd ? '<code>' + esc(t.verify_cmd) + '</code>' : '<span style="color:var(--jz-muted)">(无)</span>'}</span></div>
      <div class="kv"><span class="k">任务</span><span class="v"><pre class="jz-log" style="max-height:180px">${esc(t.prompt || '')}</pre></span></div>
      <div class="kv"><span class="k">agent 回复</span><span class="v"><pre class="jz-log" style="max-height:220px">${esc(t.agent_reply || '')}</pre></span></div>`;
  } catch (e) {
    body.innerHTML = `<span style="color:var(--jz-fail)">读取失败: ${esc(e)}</span>`;
  }
}

async function loadTasksPage() {
  const fAgent = document.getElementById('fAgent');
  const fStatus = document.getElementById('fStatus');
  const fLimit = document.getElementById('fLimit');

  if (fAgent.options.length <= 1) {
    let ids = [];
    try {
      const s = await getJson(api('status'));
      ids = (s.known || s.online || []).map(a => a.agentId);
    } catch (e) { /* ignore */ }
    ids.forEach(id => fAgent.insertAdjacentHTML('beforeend', `<option value="${esc(id)}">${esc(id)}</option>`));
  }
  const url = new URL(location.href);
  if (url.searchParams.get('agentId')) fAgent.value = url.searchParams.get('agentId');

  const qs = 'tasks?limit=' + encodeURIComponent(fLimit.value || 50) +
             (fAgent.value ? '&agentId=' + encodeURIComponent(fAgent.value) : '') +
             (fStatus.value ? '&status=' + encodeURIComponent(fStatus.value) : '');
  let data;
  try {
    data = await getJson(api(qs));
  } catch (e) {
    document.getElementById('meta').textContent = '读取失败: ' + e;
    return;
  }
  const list = data.tasks || [];
  document.getElementById('meta').textContent = `共 ${data.total || 0} 条记录，当前显示 ${list.length} 条`;
  document.getElementById('hint').textContent = '点一行看详情';
  document.getElementById('listBody').innerHTML = taskRows(list);

  document.querySelectorAll('#listBody tr[data-task]').forEach(tr => {
    tr.onclick = () => {
      const id = tr.dataset.task;
      history.replaceState(null, '', withToken('tasks?id=' + encodeURIComponent(id)));
      loadDetail(id);
    };
  });

  const id = new URLSearchParams(location.search).get('id');
  if (id) loadDetail(id);
}

/* ============================ new 页 ============================ */
async function loadNewPage() {
  const sel = document.getElementById('fAgent');
  try {
    const s = await getJson(api('status'));
    const ids = (s.online || []).map(a => a.agentId);
    sel.innerHTML = ids.length ? ids.map(id => `<option value="${esc(id)}">${esc(id)}</option>`).join('')
                               : '<option value="">(没有在线 agent)</option>';
  } catch (e) {
    sel.innerHTML = '<option value="">(读取失败)</option>';
  }

  document.getElementById('btnReset').onclick = () => {
    document.getElementById('fPrompt').value = '';
    document.getElementById('fVerify').value = '';
    document.getElementById('msg').textContent = '';
  };

  document.getElementById('btnSubmit').onclick = async () => {
    const msg = document.getElementById('msg');
    const btn = document.getElementById('btnSubmit');
    const agentId = sel.value;
    const prompt = document.getElementById('fPrompt').value.trim();
    const verify = document.getElementById('fVerify').value.trim();
    if (!agentId) { msg.textContent = '没有在线的 agent'; return; }
    if (!prompt) { msg.textContent = '任务不能为空'; return; }

    btn.disabled = true;
    msg.textContent = '提交中…';
    try {
      const body = { agentId, prompt, use_memory: false, wait: false };
      if (verify) body.verify_cmd = verify;
      const r = await fetch(api('tasks'), {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body)
      });
      const j = await r.json().catch(() => ({}));
      if (!r.ok) { msg.textContent = '失败(' + r.status + ')：' + (j.error || ''); return; }
      msg.textContent = '已提交：' + (j.taskId || '');
      location.href = withToken('tasks?id=' + encodeURIComponent(j.taskId || ''));
    } catch (e) {
      msg.textContent = '请求失败：' + e;
    } finally {
      btn.disabled = false;
    }
  };
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

  // 兜底提示: 一直停在"加载中"通常不是接口挂了, 而是浏览器还在跑缓存里的旧 app.js
  setTimeout(() => {
    const f = document.getElementById('feed');
    if (f && f.textContent.indexOf('加载中') >= 0)
      f.innerHTML = `<div class="feed__empty">4 秒还没拿到数据。<br>
        如果刚更新过页面，多半是浏览器仍在用缓存的旧脚本 —— 按 <code>Ctrl+F5</code> 强制刷新试试。</div>`;
  }, 4000);
} else if (PAGE === 'tasks') {
  loadTasksPage();
  // 不做定时刷新: 每隔两秒整页重绘会把详情读一半就刷掉(表现为一直闪)。
  // 需要最新就点「刷新」, 或者改筛选条件(自动拉一次)。
  document.getElementById('btnReload').onclick = loadTasksPage;
  ['fAgent', 'fStatus', 'fLimit'].forEach(id => {
    const el = document.getElementById(id);
    if (el) el.onchange = loadTasksPage;
  });
  document.getElementById('btnClose').onclick = () => {
    document.getElementById('detail').classList.add('hidden');
    history.replaceState(null, '', withToken('tasks'));
  };
} else if (PAGE === 'new') {
  loadNewPage();
}
