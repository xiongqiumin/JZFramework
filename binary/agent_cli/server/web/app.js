/* JZAgent 管理台: 连接式多页
     /              连接列表(每个 agent 一行, 点进详情)
     /agent?id=xxx  某个 agent 的详情 + 在这台上新建任务
     /tasks         任务记录(可筛选) + ?id= 详情
     /new           新建任务(跨 agent)
   数据: agent 实时状态 /api/status; 单个 agent /api/agent; 任务历史 /api/tasks。 */

const TOKEN = new URLSearchParams(location.search).get('token') || '';
const PAGE = document.body.dataset.page || 'agents';
const withToken = (p) => p + (TOKEN ? ((p.includes('?') ? '&' : '?') + 'token=' + encodeURIComponent(TOKEN)) : '');
const api = (p) => withToken('/api/' + p);

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
function connRow(a) {
  const nodes = a.nodes || [];
  const on = nodes.filter(n => n.online).length;
  const builtin = !!a.builtin;
  const state = a.busy
    ? '<span class="jz-badge jz-badge--info">跑任务</span>'
    : (builtin ? '<span class="jz-badge jz-badge--ok">就绪</span>' : '<span class="jz-badge jz-badge--ok">空闲</span>');
  const href = withToken('agent?id=' + encodeURIComponent(a.agentId));
  return `<a class="conn" href="${href}">
    <span class="conn__mono">${esc((a.agentId || '?').slice(0, 2).toUpperCase())}</span>
    <span class="conn__main">
      <span class="conn__id">${esc(a.agentId)} ${builtin ? '<span class="jz-badge">内置</span>' : ''}</span>
      <span class="conn__sub">${esc(a.name || '')} · v${esc(a.version || '?')}${
        a.modelName ? ' · ' + esc(a.modelName) : ''
      }${nodes.length ? ' · 节点 ' + on + '/' + nodes.length : ''}</span>
      <span class="conn__path">${esc(a.rootDir || '')}</span>
    </span>
    <span class="conn__side">
      ${state}
      <span class="conn__task">${a.taskId ? esc(a.taskId) : (a.lastEventType ? esc(a.lastEventType) : '—')}</span>
      <span class="conn__go">进入 →</span>
    </span>
  </a>`;
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
  const online = s.online || [];
  const hubCount = online.filter(a => !a.builtin).length;
  const builtinCount = online.filter(a => a.builtin).length;

  document.getElementById('meta').textContent =
    `控制面 ${hub.listening ? ('监听 ' + hub.port) : '关闭'} · 在线 ${hubCount} 台` +
    (builtinCount ? ` + 内置 ${builtinCount}` : '') +
    ` · 运行 ${((s.uptimeMs || 0) / 1000).toFixed(0)}s`;

  let tasks = [];
  try {
    tasks = (await getJson(api('tasks?limit=200'))).tasks || [];
  } catch (e) { /* 记录读不到不影响连接列表 */ }

  document.getElementById('cards').innerHTML = [
    ['在线 agent', hubCount],
    ['内置 agent', builtinCount],
    ['跑任务中', tasks.filter(t => t.status === 'running').length],
    ['任务记录', tasks.length],
  ].map(c => `<div class="jz-card kpi"><div class="jz-card__bd">` +
             `<div class="k">${c[0]}</div><div class="v">${c[1]}</div></div></div>`).join('');

  const box = document.getElementById('connList');
  box.innerHTML = online.length
    ? online.map(connRow).join('')
    : `<div class="conn conn--empty">还没有 agent 连上来。<br>
       远端: 在 agent_cli 配置里填 <code>server.host/port/token</code>；
       本机: 打开 <code>selfAgent.enabled</code>。</div>`;
}

/* ============================ agent 详情 ============================ */
async function loadAgentPage() {
  const id = new URLSearchParams(location.search).get('id') || '';
  if (!id) {
    document.getElementById('meta').textContent = '缺少 ?id=';
    return;
  }
  document.getElementById('title').textContent = id;
  document.getElementById('mono').textContent = id.slice(0, 2).toUpperCase();
  document.getElementById('allTasks').href = withToken('tasks?agentId=' + encodeURIComponent(id));

  let d;
  try {
    d = await getJson(api('agent?id=' + encodeURIComponent(id)));
  } catch (e) {
    document.getElementById('meta').textContent = '读取失败: ' + e + '（agent 可能已离线）';
    document.getElementById('infoBody').innerHTML =
      `<span style="color:var(--jz-muted)">server 上查不到这台 agent。它离线后仍会保留历史任务, 见
       <a href="${withToken('tasks?agentId=' + encodeURIComponent(id))}">任务记录</a>。</span>`;
    return;
  }

  const info = d.info || {};
  const kind = d.kind || '';
  const builtin = kind === 'builtin';
  document.getElementById('builtinNote').style.display = builtin ? '' : 'none';

  const stateText = builtin ? '内置（server 进程内）'
    : (kind === 'process' ? '本地子进程' : '远端 cli（控制面连接）');
  document.getElementById('meta').textContent =
    `${stateText} · ${info.busy ? '跑任务中' : '空闲'}` + (info.lastSeen ? ` · 最近心跳 ${fmtTime(info.lastSeen)}` : '');

  document.getElementById('infoBody').innerHTML = `
    <div class="kv"><span class="k">agentId</span><span class="v">${esc(info.agentId || id)}</span></div>
    <div class="kv"><span class="k">名称</span><span class="v">${esc(info.name || '-')} ${esc(info.version || '')}</span></div>
    <div class="kv"><span class="k">模型</span><span class="v">${esc(info.modelName || '-')}</span></div>
    <div class="kv"><span class="k">工作区</span><span class="v">${esc(info.rootDir || '-')}</span></div>
    <div class="kv"><span class="k">当前任务</span><span class="v">${
      info.taskId ? `<a href="${withToken('tasks?id=' + encodeURIComponent(info.taskId))}">${esc(info.taskId)}</a>` : '—'
    }</span></div>
    <div class="kv"><span class="k">最近事件</span><span class="v">${esc(info.lastEventType || '—')}</span></div>`;

  const nodes = info.nodes || [];
  document.getElementById('nodeBody').innerHTML = nodes.length ? nodes.map(n => `<tr>
      <td data-label="节点">${esc(n.name || '-')}</td>
      <td data-label="地址">${esc(n.host || '-')}</td>
      <td data-label="状态">${n.online ? '<span class="jz-badge jz-badge--ok">在线</span>'
                                       : '<span class="jz-badge">离线</span>'}</td>
      <td data-label="工作区" style="color:var(--jz-muted);font-size:var(--jz-fs-1)">${esc(n.rootDir || '-')}</td>
    </tr>`).join('')
    : `<tr><td colspan="4" style="color:var(--jz-muted)">没有节点（本地执行）</td></tr>`;

  const tasks = d.tasks || [];
  document.getElementById('taskBody').innerHTML = tasks.length ? tasks.map(t => `<tr style="cursor:pointer"
      onclick="location.href='${withToken('tasks?id=' + encodeURIComponent(t.taskId))}'">
      <td data-label="任务">${esc(t.taskId)}</td>
      <td data-label="状态">${badge(t.status)}</td>
      <td data-label="轮次" class="hide-sm">${t.verify_rounds || 0}</td>
      <td data-label="耗时" class="hide-sm">${fmtCost(t)}</td>
      <td data-label="提交时间" class="hide-sm">${fmtTime(t.start_ms)}</td>
    </tr>`).join('')
    : `<tr><td colspan="5" style="color:var(--jz-muted)">这台 agent 还没有任务</td></tr>`;

  document.getElementById('btnSubmit').onclick = () => submitFromDetail(id);
}

async function submitFromDetail(agentId) {
  const msg = document.getElementById('msg');
  const btn = document.getElementById('btnSubmit');
  const prompt = document.getElementById('fPrompt').value.trim();
  const verify = document.getElementById('fVerify').value.trim();
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
    location.href = withToken('tasks?id=' + encodeURIComponent(j.taskId || ''));
  } catch (e) {
    msg.textContent = '请求失败：' + e;
  } finally {
    btn.disabled = false;
  }
}

/* ============================ tasks 页 ============================ */
function taskRows(list) {
  if (!list.length) {
    return `<tr><td colspan="7" style="color:var(--jz-muted)">没有符合条件的任务</td></tr>`;
  }
  return list.map(t => `<tr data-task="${esc(t.taskId)}" style="cursor:pointer">
      <td data-label="任务">${esc(t.taskId)}</td>
      <td data-label="agent"><a href="${withToken('agent?id=' + encodeURIComponent(t.agentId))}">${esc(t.agentId)}</a></td>
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
      ids = (s.online || []).map(a => a.agentId);
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
  setInterval(loadConnectionsPage, 2000);
} else if (PAGE === 'agent') {
  loadAgentPage();
  setInterval(loadAgentPage, 3000);
} else if (PAGE === 'tasks') {
  loadTasksPage();
  setInterval(loadTasksPage, 2000);
  document.getElementById('btnReload').onclick = loadTasksPage;
  document.getElementById('btnClose').onclick = () => {
    document.getElementById('detail').classList.add('hidden');
    history.replaceState(null, '', withToken('tasks'));
  };
} else if (PAGE === 'new') {
  loadNewPage();
}
