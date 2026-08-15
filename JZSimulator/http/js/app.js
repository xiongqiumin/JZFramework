// JZSimServer 控制台前端逻辑
// 左侧列表 = 运行期实际 open 的驱动实例(服务端 /sim/state 的 associations),
// 开几个实例就显示几个; 按类别分组。离线(file://)无 associations, 显示全部类别作预览。
// 点击后在右侧 iframe 加载 drivers/<category>.html#<id>(id 用 URL hash 定位实例)。

const API = {
    state: '/sim/state',
    config: '/sim/config',
};

const LABELS = {
    gpio:   'GPIO 驱动',
    ble:    'BLE 驱动',
    wifi:   'WiFi 驱动',
    sensor: 'Sensor 驱动',
    ota:    'OTA 驱动',
};

// 离线兜底: 列出全部类别(无实例, 仅预览)
const ALL_DRIVERS = ['gpio', 'ble', 'wifi', 'sensor', 'ota'];

function loadState() {
    return fetch(API.state)
        .then(r => r.json())
        .then(j => j.state)
        .catch(() => null);
}

// items: [{cat, id, label}]  (在线为实例, 离线为类别占位)
function renderList(items, activeKey) {
    const ul = document.getElementById('driver-list');
    ul.innerHTML = '';
    if (!items || items.length === 0) {
        const li = document.createElement('li');
        li.className = 'placeholder';
        li.textContent = '无可用驱动';
        ul.appendChild(li);
        return;
    }
    // 按类别分组
    const groups = {};
    items.forEach(it => {
        (groups[it.cat] = groups[it.cat] || []).push(it);
    });
    Object.keys(groups).forEach(cat => {
        const g = document.createElement('li');
        g.className = 'group';
        g.textContent = LABELS[cat] || cat;
        ul.appendChild(g);
        groups[cat].forEach(it => {
            const key = cat + ':' + it.id;
            const li = document.createElement('li');
            if (key === activeKey) li.className = 'active';
            li.dataset.cat = cat;
            li.dataset.id = it.id;
            li.innerHTML = '<span class="dot"></span>' + it.label;
            li.addEventListener('click', () => selectDriver(cat, it.id));
            ul.appendChild(li);
        });
    });
}

function selectDriver(cat, id) {
    // 网页根路径为 /, 子页面走相对路径; #<id> 让子页定位实例
    document.getElementById('panel').src = 'drivers/' + cat + '.html#' + encodeURIComponent(id || '');
    document.querySelectorAll('#driver-list li').forEach(li => {
        const on = li.dataset.cat === cat && (li.dataset.id || '') === (id || '');
        li.classList.toggle('active', on);
    });
}

function itemsFromAssociations(assocs) {
    // associations: [{category, id, label}] -> [{cat, id, label}]
    return (assocs || []).map(a => ({ cat: a.category, id: a.id, label: a.label }));
}

async function refresh() {
    const st = await loadState();
    const conn = document.getElementById('conn-state');
    if (!st) {
        // 离线(file:// 直接打开): 显示全部类别占位, 仅标记已断开
        conn.textContent = '● 已断开';
        conn.classList.add('off');
        const items = ALL_DRIVERS.map(cat => ({ cat, id: '', label: LABELS[cat] || cat }));
        const cur = document.querySelector('#driver-list li.active');
        const activeKey = cur ? (cur.dataset.cat + ':' + (cur.dataset.id || '')) : null;
        renderList(items, activeKey);
        if (!cur && items.length) selectDriver(items[0].cat, '');
        return;
    }
    conn.textContent = '● 已连接';
    conn.classList.remove('off');
    // 在线: 列表 = 实际 open 的实例(开几个显示几个)
    const items = itemsFromAssociations(st.associations);
    const cur = document.querySelector('#driver-list li.active');
    const activeKey = cur ? (cur.dataset.cat + ':' + (cur.dataset.id || '')) : null;
    renderList(items, activeKey);
    if (!cur && items.length) selectDriver(items[0].cat, items[0].id);
}

// 每 2s 刷新一次(驱动实例随运行期 open/close 变化)
refresh();
setInterval(refresh, 2000);

// ==================== 外围设备配置 ====================
// 加载配置: 右侧打开 drivers/config.html(自包含编辑页, 自动拉取服务端当前配置)
// 保存配置: 触发右侧 config 页的 saveConfig()(若右侧不是 config 页则先打开再保存)
function openConfigPage() {
    document.getElementById('panel').src = 'drivers/config.html';
}
function configPage() {
    const f = document.getElementById('panel');
    if (f && f.contentWindow && f.contentWindow.saveConfig) return f.contentWindow;
    return null;
}
document.getElementById('btn-config-load').addEventListener('click', openConfigPage);
document.getElementById('btn-config-save').addEventListener('click', () => {
    const p = configPage();
    if (p) p.saveConfig();
    else { openConfigPage(); setTimeout(() => { const q = configPage(); if (q) q.saveConfig(); }, 300); }
});
