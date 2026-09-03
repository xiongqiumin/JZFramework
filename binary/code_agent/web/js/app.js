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
    const sp = document.createElement('button');
    sp.className = 'speak-btn';
    sp.textContent = '🔊';
    sp.title = '朗读此回复';
    sp.onclick = () => speakText(raw);
    pendingAssistant.appendChild(sp);
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
    showModal('configModal');   // 先弹出, 再异步填值, 避免接口慢/失败时“点不开”
    try {
        const data = await api('/api/config');
        $('cfgUrl').value = data.modelUrl || '';
        $('cfgName').value = data.modelName || '';
        $('cfgToken').value = data.modelToken || '';
        $('cfgPort').value = data.port || 18080;
        $('cfgSpeechWs').value = data.speechWs || '';
        $('cfgLocalPlay').checked = !!data.localPlay;
    } catch (e) {
        console.error('加载配置失败', e);
    }
}

async function saveConfig() {
    const res = await postJson('/api/config', {
        modelUrl: $('cfgUrl').value,
        modelName: $('cfgName').value,
        modelToken: $('cfgToken').value,
        port: parseInt($('cfgPort').value, 10) || 18080,
        speechWs: $('cfgSpeechWs').value.trim(),
        localPlay: $('cfgLocalPlay').checked
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
    $('btnMic').onclick = onMic;
    $('btnSpeak').onclick = onSpeak;

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

/* ---------------- 语音: 录音(ASR) + 朗读(TTS) ----------------
 * 后端 /api/asr、/api/tts 对接 ugreen-server (见 JZSpeechClient)。
 * 录音: 浏览器用 Web Audio 采集麦克风采样, 重采样到 16k 单声道 16bit PCM 后 base64 上传。
 */
let recState = null;   // { stream, ctx, proc, pcm: Float32Array[] }

function bytesToBase64(bytes) {   // bytes: Uint8Array -> base64 字符串
    let bin = '';
    const chunk = 0x8000;
    for (let i = 0; i < bytes.length; i += chunk)
        bin += String.fromCharCode.apply(null, bytes.subarray(i, i + chunk));
    return btoa(bin);
}

function base64ToBytes(b64) {
    const bin = atob(b64);
    const u8 = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
    return u8;
}

// 线性插值重采样 (语音够用)
function resample(input, fromRate, toRate) {
    const ratio = fromRate / toRate;
    const newLen = Math.round(input.length / ratio);
    const out = new Float32Array(newLen);
    for (let i = 0; i < newLen; i++) {
        const idx = i * ratio;
        const i0 = Math.floor(idx);
        const i1 = Math.min(i0 + 1, input.length - 1);
        const frac = idx - i0;
        out[i] = input[i0] * (1 - frac) + input[i1] * frac;
    }
    return out;
}

async function startRecording() {
    const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
    const Ctx = window.AudioContext || window.webkitAudioContext;
    const ctx = new Ctx({ sampleRate: 16000 });
    const src = ctx.createMediaStreamSource(stream);
    const proc = ctx.createScriptProcessor(4096, 1, 1);
    const pcm = [];
    proc.onaudioprocess = (e) => {
        pcm.push(Float32Array.from(e.inputBuffer.getChannelData(0)));
    };
    src.connect(proc);
    proc.connect(ctx.destination);   // 必须连到 destination 才会触发 onaudioprocess
    recState = { stream, ctx, proc, pcm };
    $('btnMic').textContent = '■ 停止';
    $('btnMic').classList.add('rec');
}

function stopRecording() {
    if (!recState) return null;
    const { stream, ctx, proc, pcm } = recState;
    try { proc.disconnect(); ctx.suspend(); } catch (e) {}
    stream.getTracks().forEach((t) => t.stop());

    let total = 0;
    pcm.forEach((a) => total += a.length);
    const merged = new Float32Array(total);
    let off = 0;
    pcm.forEach((a) => { merged.set(a, off); off += a.length; });

    // 浏览器可能不支持 16k 采样率, 这里重采样到 16k
    let out = merged;
    if (ctx.sampleRate !== 16000)
        out = resample(merged, ctx.sampleRate, 16000);

    const int16 = new Int16Array(out.length);
    for (let i = 0; i < out.length; i++) {
        const s = Math.max(-1, Math.min(1, out[i]));
        int16[i] = s < 0 ? s * 0x8000 : s * 0x7fff;
    }
    recState = null;
    $('btnMic').textContent = '🎤 录音';
    $('btnMic').classList.remove('rec');
    return bytesToBase64(new Uint8Array(int16.buffer));
}

async function onMic() {
    if (recState) {
        const b64 = stopRecording();
        if (b64) await sendAsr(b64);
    } else {
        try { await startRecording(); }
        catch (e) { alert('无法访问麦克风: ' + (e && e.message ? e.message : e)); }
    }
}

async function sendAsr(b64) {
    $('btnMic').disabled = true;
    try {
        const res = await postJson('/api/asr', { audio: b64 });
        if (res.ok) {
            $('input').value = res.text;
            $('input').focus();
        } else {
            alert(res.error || '识别失败');
        }
    } catch (e) {
        alert('识别请求失败: ' + e);
    } finally {
        $('btnMic').disabled = false;
    }
}

async function onSpeak() {
    let text = $('input').value.trim();
    if (!text) {
        const el = document.querySelector('.bubble.assistant');
        if (el) text = el._raw || el.textContent;
    }
    if (text) await speakText(text);
}

async function speakText(text) {
    $('btnSpeak').disabled = true;
    try {
        const res = await postJson('/api/tts', { text });
        if (res.ok) {
            const blob = new Blob([base64ToBytes(res.audio)], { type: 'audio/wav' });
            const url = URL.createObjectURL(blob);
            const a = new Audio(url);
            a.play();
            a.onended = () => URL.revokeObjectURL(url);
        } else {
            alert(res.error || '合成失败');
        }
    } catch (e) {
        alert('合成请求失败: ' + e);
    } finally {
        $('btnSpeak').disabled = false;
    }
}

init();
