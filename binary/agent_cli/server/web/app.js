/* JZAgentCliServer 管理台前端: 轮询 /api/* 显示 agent 状态与日志。
   ⚠ 类名统一用 jz-ui 的组件约定(见 /ui/components.css), 不在这里造新样式。 */

const params = new URLSearchParams(location.search);
const TOKEN = params.get('token') || '';
const api = (p) => '/api/' + p +
  (TOKEN ? (p.includes('?') ? '&' : '?') + 'token=' + encodeURIComponent(TOKEN) : '');

let selected = null;

async function load() {
  try {
    const r = await fetch(api('status'));
    if (!r.ok) {
      document.getElementById('meta').textContent = '未授权/错误 ' + r.status;
      return;
    }
    const s = await r.json();
    renderMeta(s);
    renderCards(s);
    renderOnline(s);
    renderTable(s);
  } catch (e) {
    document.getElementById('meta').textContent = '请求失败: ' + e;
  }
}

function renderMeta(s) {
  const up = ((s.uptimeMs || 0) / 1000).toFixed(0);
  const hub = s.hub || {};
  document.getElementById('meta').textContent =
    `frps=${s.frp.enabled ? '开' : '关'}  网页=http://${s.http.bind}:${s.http.port}` +
    `  控制面=${hub.listening ? ('监听 ' + hub.port) : '关'}  在线 cli ${(s.online || []).length}` +
    `  子进程 ${s.agents.length}  运行 ${up}s`;
}

// 控制面上线的 cli: 一行一台, 节点列显示 node 在线数/总数
function renderOnline(s) {
  const tb = document.querySelector('#onlineTbl tbody');
  const list = s.online || [];
  const dim = 'color:var(--jz-muted)';
  if (!list.length) {
    tb.innerHTML = `<tr><td colspan="6" style="${dim}">还没有 cli 连上来（在 agent_cli 配置里填 server.host/port/token）</td></tr>`;
    return;
  }
  tb.innerHTML = list.map(a => {
    const nodes = a.nodes || [];
    const on = nodes.filter(n => n.online).length;
    const state = a.busy
      ? `<span class="jz-badge jz-badge--info">跑 ${esc(a.taskId || '')}</span>`
      : '<span class="jz-badge">空闲</span>';
    const ev = a.lastEventType ? esc(a.lastEventType) : `<span style="${dim}">-</span>`;
    return `<tr><td data-label="agentId">${esc(a.agentId)}</td>` +
           `<td data-label="名称">${esc(a.name)} <span style="${dim}">v${esc(a.version)}</span></td>` +
           `<td data-label="状态">${state}</td>` +
           `<td data-label="节点">${nodes.length ? (on + '/' + nodes.length) : `<span style="${dim}">本地</span>`}</td>` +
           `<td data-label="最近事件">${ev}</td>` +
           `<td data-label="工作区" style="${dim}">${esc(a.rootDir || '')}</td></tr>`;
  }).join('');
}

function renderCards(s) {
  const running = s.agents.filter(a => a.running).length;
  const cards = [
    ['Agents', s.agents.length],
    ['运行中', running],
    ['在线 cli', (s.online || []).length],
    ['运行时间', ((s.uptimeMs || 0) / 1000).toFixed(0) + 's'],
  ];
  document.getElementById('cards').innerHTML = cards.map(c =>
    `<div class="jz-card kpi"><div class="jz-card__bd">` +
    `<div class="k">${c[0]}</div><div class="v">${c[1]}</div></div></div>`).join('');
}

function renderTable(s) {
  const tb = document.querySelector('#tbl tbody');
  tb.innerHTML = '';
  const badge = { running: 'jz-badge--ok', starting: 'jz-badge--warn' };
  s.agents.forEach(a => {
    const tr = document.createElement('tr');
    tr.dataset.agent = a.name;
    if (a.name === selected) tr.className = 'is-sel';
    tr.innerHTML =
      `<td data-label="名称">${esc(a.name)}</td>` +
      `<td data-label="状态"><span class="jz-badge ${badge[a.state] || ''}">${esc(a.state)}</span></td>` +
      `<td data-label="退出码">${a.exitCode}</td>` +
      `<td data-label="命令" style="color:var(--jz-muted);font-size:var(--jz-fs-1)">${esc(a.cmd + ' ' + (a.args || []).join(' '))}</td>` +
      `<td data-label="日志行">${a.logLines}</td>` +
      `<td data-label="操作">
         <button class="jz-btn jz-btn--sm" onclick="act('start','${esc(a.name)}',event)">启动</button>
         <button class="jz-btn jz-btn--sm" onclick="act('stop','${esc(a.name)}',event)">停止</button>
         <button class="jz-btn jz-btn--sm jz-btn--danger" onclick="act('restart','${esc(a.name)}',event)">重启</button>
       </td>`;
    tr.onclick = (e) => {
      if (e.target.tagName === 'BUTTON') return;
      selected = a.name;
      loadLog();
      load();
    };
    tb.appendChild(tr);
  });
  if (selected && !s.agents.find(a => a.name === selected)) selected = null;
}

async function act(action, name, e) {
  e.stopPropagation();
  await fetch(api(`agents/${action}?name=${encodeURIComponent(name)}`), { method: 'POST' });
  load();
  if (action !== 'stop') setTimeout(loadLog, 600);
}

async function loadLog() {
  if (!selected) return;
  document.getElementById('logName').textContent = '日志: ' + selected;
  const pre = document.getElementById('log');
  try {
    const r = await fetch(api(`agents/logs?name=${encodeURIComponent(selected)}&lines=300`));
    if (!r.ok) {
      pre.textContent = '(无日志/未授权)';
      return;
    }
    pre.textContent = (await r.text()) || '(空)';
    pre.scrollTop = pre.scrollHeight;
  } catch (e) {
    pre.textContent = '日志请求失败: ' + e;
  }
}

function esc(s) {
  return String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}

document.getElementById('copyToken').onclick = () => {
  const url = location.origin + '/?token=' + encodeURIComponent(TOKEN);
  navigator.clipboard?.writeText(url);
  const btn = document.getElementById('copyToken');
  btn.textContent = '已复制';
  setTimeout(() => { btn.textContent = '复制带 token 的链接'; }, 1500);
};

load();
setInterval(load, 2000);
setInterval(() => { if (selected) loadLog(); }, 1500);
