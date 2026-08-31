/* JZCodeAgent 网页端
 * 与后端的交互很简单：POST /api/chat 提交任务，然后按 id 增量轮询 /api/state 拉取会话日志。
 */
'use strict';

const $ = (id) => document.getElementById(id);

let since = 0;
let busy = false;
let pendingAssistant = null;   // 正在流式输出的助手气泡
let pendingTool = null;        // 最近一次工具调用（等待它的返回）
let curPath = '';              // 文件浏览当前目录（相对工作区）

/* ---------------- 工具函数 ---------------- */
async function api(url, options) {
    const res = await fetch(url, options);
    return await res.json();
}

function postJson(url, obj) {
    return api(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(obj)
    });
}

function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, (c) => ({
        '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
    }[c]));
}

// 极简 markdown：代码块 / 行内代码 / 加粗 / 换行
function renderMd(src) {
    const blocks = [];
    let text = escapeHtml(src);
    text = text.replace(/```([a-zA-Z0-9_+-]*)\n([\s\S]*?)```/g, (_m, _lang, code) => {
        blocks.push(code.replace(/\n$/, ''));
        return '\u0000PRE' + (blocks.length - 1) + '\u0000';
    });
    text = text.replace(/`([^`\n]+)`/g, '<code>$1</code>');
    text = text.replace(/\*\*([^*\n]+)\*\*/g, '<strong>$1</strong>');
    text = text.replace(/\n/g, '<br>');
    text = text.replace(/\u0000PRE(\d+)\u0000/g, (_m, i) => '<pre><code>' + blocks[Number(i)] + '</code></pre>');
    return text;
}

function addEl(el) {
    $('chat').appendChild(el);
    scrollBottom();
}

function caretSpan() {
    const s = document.createElement('span');
    s.className = 'caret';
    return s;
}

function scrollBottom(force) {
    const chat = $('chat');
    if (force || chat.scrollHeight - chat.scrollTop - chat.clientHeight < 240) {
        chat.scrollTop = chat.scrollHeight;
    }
}

/* ---------------- 消息渲染 ---------------- */
function bubble(kind, text) {
    const el = document.createElement('div');
    el.className = 'msg';
    const b = document.createElement('div');
    b.className = 'bubble ' + kind;
    b.innerHTML = renderMd(text);
    el.appendChild(b);
    return el;
}

function appendAssistantDelta(text) {
    if (!pendingAssistant) {
        const el = document.createElement('div');
        el.className = 'msg';
        const b = document.createElement('div');
        b.className = 'bubble assistant';
        b._raw = '';
        el.appendChild(b);
        $('chat').appendChild(el);
        pendingAssistant = b;
        pendingTool = null;
    }
    pendingAssistant._raw += text;
    pendingAssistant.textContent = pendingAssistant._raw;
    pendingAssistant.appendChild(caretSpan());
    scrollBottom();
}

// 流式结束后把累积的纯文本渲染成 markdown
function finalizeAssistant() {
    if (!pendingAssistant) return;
    const raw = pendingAssistant._raw || '';
    pendingAssistant.innerHTML = renderMd(raw);
    pendingAssistant = null;
}

function createToolCard(name, args, result) {
    const card = document.createElement('div');
    card.className = 'msg';
    const box = document.createElement('div');
    box.className = 'tool';

    const head = document.createElement('div');
    head.className = 'tool-head';
    const tag = document.createElement('span');
    tag.className = 'tag';
    tag.textContent = '⚙ ' + name;
    const a = document.createElement('span');
    a.className = 'args';
    a.textContent = String(args || '').split('\n')[0];
    const st = document.createElement('span');
    st.className = 'state';
    st.textContent = '执行中…';
    head.appendChild(tag);
    head.appendChild(a);
    head.appendChild(st);

    const body = document.createElement('pre');
    body.className = 'tool-body';

    head.onclick = () => box.classList.toggle('open');
    box.appendChild(head);
    box.appendChild(body);
    card.appendChild(box);

    const obj = {
        card,
        setResult(text) {
            body.textContent = text || '';
            const t = String(text || '').trim();
            const first = t.split('\n')[0] || '';
            if (/退出码:\s*0\b/.test(t)) st.textContent = '成功';
            else if (first) st.textContent = first.slice(0, 48);
            else st.textContent = '完成';
            box.classList.add('open');
            scrollBottom();
        }
    };
    if (result !== undefined) obj.setResult(result);
    return obj;
}

function renderLog(log) {
    if (log.type === 'user') {
        finalizeAssistant();
        pendingTool = null;
        addEl(bubble('user', log.text));
    } else if (log.type === 'delta') {
        appendAssistantDelta(log.text);
    } else if (log.type === 'tool_call') {
        finalizeAssistant();
        pendingTool = createToolCard(log.name, log.text);
        addEl(pendingTool.card);
    } else if (log.type === 'tool_result') {
        finalizeAssistant();
        if (pendingTool) {
            pendingTool.setResult(log.text);
            pendingTool = null;
        } else {
            addEl(createToolCard(log.name, '', log.text).card);
        }
    } else if (log.type === 'error') {
        finalizeAssistant();
        pendingTool = null;
        addEl(bubble('error', log.text));
    } else if (log.type === 'info') {
        finalizeAssistant();
        pendingTool = null;
        addEl(bubble('info', log.text));
    }
}

function clearChat() {
    $('chat').innerHTML = '';
    pendingAssistant = null;
    pendingTool = null;
}

/* ---------------- 状态轮询 ---------------- */
function setBusy(v) {
    if (busy && !v) finalizeAssistant();
    busy = v;
    $('statusDot').className = 'dot ' + (v ? 'busy' : 'idle');
    $('btnStop').disabled = !v;
    $('btnSend').disabled = v;
}

function updateMeta(data) {
    const u = data.usage || {};
    $('meta').textContent = data.workspace
        ? data.workspace + '  ·  tokens: ' + (u.total_tokens || 0)
        : '';
}

async function poll() {
    let data;
    try {
        data = await api('/api/state?since=' + since);
    } catch (e) {
        return;
    }

    // 服务端清空了会话（id 归零）
    if (typeof data.seq === 'number' && data.seq < since) {
        clearChat();
        since = 0;
    }

    if (data.logs && data.logs.length) {
        for (const log of data.logs) {
            renderLog(log);
            if (log.id > since) since = log.id;
        }
    }
    setBusy(!!data.busy);
    updateMeta(data);
}

/* ---------------- 文件浏览 ---------------- */
function fileItem(name, isDir, target, isUp) {
    const li = document.createElement('li');
    li.className = isDir ? (isUp ? 'up' : 'dir') : '';
    const ico = document.createElement('span');
    ico.className = 'ico';
    ico.textContent = isUp ? '↑' : (isDir ? '▸' : '·');
    const label = document.createElement('span');
    label.textContent = name;
    li.appendChild(ico);
    li.appendChild(label);
    li.onclick = () => {
        if (isDir) loadFiles(target);
        else openFile(target);
    };
    return li;
}

async function loadFiles(path) {
    let data;
    try {
        data = await api('/api/files?path=' + encodeURIComponent(path || ''));
    } catch (e) {
        return;
    }
    const ul = $('fileList');
    ul.innerHTML = '';
    if (!data.ok) {
        const li = document.createElement('li');
        li.className = 'empty';
        li.textContent = data.error || '读取失败';
        ul.appendChild(li);
        return;
    }

    curPath = path || '';
    $('curPath').textContent = curPath || '.';

    if (curPath) {
        const segs = curPath.split('/');
        segs.pop();
        ul.appendChild(fileItem('..', true, segs.join('/'), true));
    }
    if (!data.items.length) {
        const li = document.createElement('li');
        li.className = 'empty';
        li.textContent = '（空目录）';
        ul.appendChild(li);
        return;
    }
    for (const it of data.items) {
        const target = curPath ? curPath + '/' + it.name : it.name;
        ul.appendChild(fileItem(it.name, it.isDir, target));
    }
}

async function openFile(path) {
    const data = await api('/api/file?path=' + encodeURIComponent(path));
    if (!data.ok) {
        alert(data.error || '读取失败');
        return;
    }
    $('fileTitle').textContent = path + (data.truncated ? '（已截断）' : '');
    $('fileContent').textContent = data.content;
    showModal('fileModal');
}

/* ---------------- 弹窗 ---------------- */
function showModal(id) { $(id).classList.remove('hidden'); }
function hideModal(id) { $(id).classList.add('hidden'); }

async function openConfig() {
    const data = await api('/api/config');
    $('cfgUrl').value = data.modelUrl || '';
    $('cfgName').value = data.modelName || '';
    $('cfgToken').value = data.modelToken || '';
    $('cfgWorkspace').value = data.workspace || '';
    $('cfgBuild').value = data.buildCommand || '';
    $('cfgRun').value = data.runCommand || '';
    showModal('configModal');
}

async function saveConfig() {
    const res = await postJson('/api/config', {
        modelUrl: $('cfgUrl').value,
        modelName: $('cfgName').value,
        modelToken: $('cfgToken').value,
        workspace: $('cfgWorkspace').value,
        buildCommand: $('cfgBuild').value,
        runCommand: $('cfgRun').value
    });
    if (!res.ok) {
        alert(res.error || '保存失败');
        return;
    }
    hideModal('configModal');
    loadFiles('');
}

/* ---------------- 交互 ---------------- */
async function send() {
    const input = $('input');
    const text = input.value.trim();
    if (!text || busy) return;
    input.value = '';
    const res = await postJson('/api/chat', { message: text });
    if (!res.ok) alert(res.error || '发送失败');
    await poll();
}

async function stopTask() {
    await postJson('/api/stop', {});
    await poll();
}

async function clearSession() {
    await postJson('/api/clear', {});
    clearChat();
    since = 0;
    await poll();
}

function init() {
    $('btnSend').onclick = send;
    $('btnStop').onclick = stopTask;
    $('btnClear').onclick = clearSession;
    $('btnConfig').onclick = openConfig;
    $('btnSaveConfig').onclick = saveConfig;
    $('btnRefresh').onclick = () => loadFiles(curPath);

    $('input').addEventListener('keydown', (e) => {
        if (e.key === 'Enter' && !e.shiftKey) {
            e.preventDefault();
            send();
        }
    });

    for (const el of document.querySelectorAll('[data-close]')) {
        el.onclick = () => hideModal(el.getAttribute('data-close'));
    }
    for (const id of ['configModal', 'fileModal']) {
        $(id).addEventListener('click', (e) => {
            if (e.target === $(id)) hideModal(id);
        });
    }
    document.addEventListener('keydown', (e) => {
        if (e.key === 'Escape') {
            hideModal('configModal');
            hideModal('fileModal');
        }
    });

    loadFiles('');
    poll();
    setInterval(poll, 400);
}

init();
