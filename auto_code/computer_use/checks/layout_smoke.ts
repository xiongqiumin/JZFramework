// checks/layout_smoke.ts —— **布局通道 + 本地分级**的离线自检
//
// 为什么能离线跑（不需要模型、不需要真桌面、不需要 C++ 侧）：
//   布局通道（按路径寻址 / 读状态 / 页面指纹 / 失败分级）全是**纯逻辑** ——
//   只要用 setBackend() 注入一个"假的 __jz.computeruse"，整条**运行期**链路就能跑一遍。
//   （这正是 docs/compute-use-design.md §4.1 的价值：运行期不涉像素 ⇒ 才可能离线验证。）
//
// 用例形状是真实的：**打开 / 关闭电源**（外部电源软件的界面自动化）。
//
// 运行: code_runtime --entry auto_code/computer_use/build/checks/layout_smoke.js
// 产物: runs/current/psu.json（建模固化出来的模型，含 layout 路径与页面指纹）
//
// 硬门（全过才 PASS；对齐 verify.py 的"硬门齐过"口径，不做加权总分）：
//   G1  建模：perception + layout ⇒ 每个元素都认领到布局路径
//   G2  固化：save → 文件 → load，布局路径与页面指纹都还在
//   G3  绑定：attach ⇒ bound=4 / 无退化 / 无缺失 / pageMatch=1
//   G4  ⭐ **不涉像素**：一次完整"开→关"里 match 收到的 locator 全是 kind='layout'
//   G5  开/关电源：点击序列与断言（ON / OFF）都对
//   G6  偶发(A)：第 1 次找不到 ⇒ 判 transient（重试即可，不问模型）
//   G7  真错·换页(D)：稳定复现 + 指纹对不上 ⇒ page-miss / ask-model
//   G8  真错·漂移(E)：同页但路径变了 ⇒ path-drift / ask-model
//   G9  前置缺失(B)：节点在但 enabled=false ⇒ precondition；且第 1 次先判偶发
//   G10 弹窗(C)：布局里多出顶层节点 ⇒ popup / dismiss-popup
//   G11 无布局：整树显式 degraded；退化后走几何兜底、不崩、也不假装有布局
import fs from 'fs';
import { App, setBackend, backend as defaultBackend, flatten, formatDiagnosis, diagnose } from 'computeruse';
import type { Diagnosis, LayoutNode, Locator, Rect, Perception } from 'computeruse';

declare var jzApp: { applicationDirPath(): string };

// —— 运行根 = 本 js 所在目录(build/checks) ⇒ 任务根 = 上两级（与 probe/scenes 同一约定）——
const runDir = jzApp.applicationDirPath();
const taskDir = runDir + '/../..';
const outDir = taskDir + '/runs/current';

// ————————————————————————————————————————————————
// 硬门
// ————————————————————————————————————————————————
const gates: Array<{ name: string; ok: boolean; detail: string }> = [];
function gate(name: string, ok: boolean, detail?: string): void {
    gates.push({ name, ok, detail: detail || '' });
    console.log((ok ? '[PASS] ' : '[FAIL] ') + name + (detail ? '  -- ' + detail : ''));
}

// ————————————————————————————————————————————————
// 夹具：一台电源软件的"界面"（布局树）+ 它的感知结果
// ————————————————————————————————————————————————
interface Scene {
    out: 'ON' | 'OFF';
    renameBtn?: boolean;    // 按钮改了 viewId（版本漂移）
    hideBtn?: boolean;      // 按钮暂时不在布局里（还没渲染出来）
    disableBtn?: boolean;   // 按钮在，但被禁用（前置条件没满足）
    popup?: boolean;        // 多出一个对话框
    otherApp?: boolean;     // 换成了完全另一个界面（走错流程）
}

const R = (x: number, y: number, w: number, h: number): Rect => ({ x, y, w, h });

function layoutOf(s: Scene): LayoutNode {
    if (s.otherApp) {
        // 另一个程序/另一个页面：关键节点几乎全不一样
        return {
            role: 'window', id: 'main', text: '示波器', rect: R(0, 0, 1, 1), visible: true,
            children: [
                { role: 'button', id: 'btnRun', text: '运行', rect: R(0.05, 0.10, 0.2, 0.08), visible: true },
                { role: 'label', id: 'lblTrig', text: '触发: 上升沿', rect: R(0.3, 0.1, 0.2, 0.08), visible: true },
            ],
        };
    }
    const kids: LayoutNode[] = [];
    if (!s.hideBtn) {
        kids.push({
            role: 'button', id: s.renameBtn ? 'btnOutputNew' : 'btnOutput', text: '输出开关',
            rect: R(0.05, 0.10, 0.20, 0.08), visible: true, clickable: true,
            enabled: !s.disableBtn,
        });
    }
    kids.push({ role: 'label', id: 'lblOutput', text: s.out, rect: R(0.30, 0.10, 0.12, 0.08), visible: true });
    kids.push({ role: 'textbox', id: 'editVoltage', text: '5.0', rect: R(0.05, 0.30, 0.20, 0.06), visible: true, enabled: true });
    if (s.popup)
        kids.push({ role: 'dialog', id: 'dlgWarn', text: '确认关闭输出?', rect: R(0.35, 0.35, 0.30, 0.20), visible: true });
    return { role: 'window', id: 'main', text: 'PSU-3000', rect: R(0, 0, 1, 1), visible: true, children: kids };
}

/** 建模期的感知结果（"VLM 看到的三个控件"）。 */
const PERCEPTION: Perception = {
    title: '',
    elements: [
        { type: 'button', text: '输出开关', rect: R(0.05, 0.10, 0.20, 0.08) },
        { type: 'label', text: 'OFF', rect: R(0.30, 0.10, 0.12, 0.08) },
        { type: 'edit', text: '5.0', rect: R(0.05, 0.30, 0.20, 0.06) },
    ],
};

// ————————————————————————————————————————————————
// 假后端：只提供"设备/布局原语"，JS 侧实现（树/寻址/校验/分级）全部保留
// ————————————————————————————————————————————————
let scene: Scene = { out: 'OFF' };
let live: LayoutNode | null = layoutOf(scene);
let matchLog: Locator[] = [];
let clicks: string[] = [];
let launches = 0;

// 模型固化走**默认实现**（真 serialize/deserialize）：给它 `__jz.core.writeFile/readFile` 就够。
//   ⇒ G2 验证的是包内那条真实往返链路，而不是夹具自己拼的 JSON。
// ⚠ 只在**缺**的时候补：`__jz.core` 是引擎的（fs 模块的 mkdirSync/readFileSync 都靠它），
//   整个覆盖掉会连 `dirMkdir` 一起干掉（第一版就踩了这个：fs.mkdirSync 报 not a function）。
{
    const g: any = globalThis;
    const core: any = (g.__jz && g.__jz.core) || {};
    if (!core.writeFile) core.writeFile = (p: string, s: string): void => { fs.writeFileSync(p, s, 'utf8'); };
    if (!core.readFile) core.readFile = (p: string): any => fs.readFileSync(p);
    g.__jz = { ...(g.__jz || {}), core };
}

function nodeByPath(path: string): LayoutNode | null {
    if (!live) return null;
    for (const e of flatten(live)) if (e.path === path) return e.node;
    return null;
}
function pathByRect(r: Rect): string {
    if (!live) return '';
    for (const e of flatten(live)) {
        const n = e.node.rect;
        if (n && Math.abs(n.x - r.x) < 1e-6 && Math.abs(n.y - r.y) < 1e-6) return e.path;
    }
    return '';
}

setBackend({
    ...(defaultBackend as any),                                  // saveModel / loadModel 用默认实现
    screenshot: async () => 'data:image/png;base64,iVBORw0KGgo=',
    layoutTree: async () => live,
    waitStable: async () => true,
    wait: async () => { },
    launch: async () => { launches++; return true; },
    match: async (loc: Locator): Promise<Rect | null> => {
        matchLog.push(loc);
        if (loc.kind === 'layout') {
            const n = nodeByPath(loc.path);
            return n && n.rect ? n.rect : null;
        }
        return { x: 0.9, y: 0.9, w: 0.01, h: 0.01 };              // 像素族（只该在退化兜底里出现）
    },
    click: async (r: Rect): Promise<boolean> => {
        const p = pathByRect(r);
        clicks.push(p || '(不在布局里:几何兜底)');
        if (scene.popup) return true;                            // 被对话框挡住：点到了，但不生效
        if (p === '/main/btnOutput' || p === '/main/btnOutputNew') {
            scene = { out: scene.out === 'OFF' ? 'ON' : 'OFF' };
            live = layoutOf(scene);                              // 点完界面就变了
        }
        return true;
    },
    drag: async () => true,
    key: async () => { },
    type: async () => { },
} as any);

// ————————————————————————————————————————————————
// 主流程
// ————————————————————————————————————————————————
async function failing(err: unknown, app: any, path: string, attempt: number, expectMatcher?: any): Promise<Diagnosis> {
    return await app.diagnose(err, { path, attempt, maxAttempts: 3, expect: expectMatcher });
}

async function main(): Promise<void> {
    fs.mkdirSync(outDir, { recursive: true });

    // ---- G1 建模：感知 + 布局 ⇒ 认领出布局路径 ----
    scene = { out: 'OFF' }; live = layoutOf(scene);
    const app: any = App.build(PERCEPTION, { layout: live });
    const nodes = app.nodes();
    const paths = nodes.map((n: any) => n.layout);
    gate('G1 建模：每个元素都认领到布局路径',
        nodes.length === 4 && nodes.every((n: any) => !!n.layout),
        'nodes=' + nodes.length + ' paths=' + JSON.stringify(paths));

    // ---- G2 固化：真 serialize → 文件 → 真 deserialize ----
    const modelPath = outDir + '/psu.json';
    await app.save(modelPath);
    const app2: any = await App.load(modelPath);
    let atOk = true;
    try { app2.at('/main/btnOutput'); } catch (e) { atOk = false; }
    const fp = app2.fingerprint;
    gate('G2 固化往返：布局路径与页面指纹都保留',
        atOk && !!fp && fp.paths.length >= 3 && fs.existsSync(modelPath),
        'model=' + modelPath + ' fingerprint=' + JSON.stringify(fp));

    // ---- G3 绑定布局 ----
    const att = await app2.attach(live);
    gate('G3 attach：全部认领、无退化、无缺失',
        att.bound === 4 && att.degraded.length === 0 && att.missing.length === 0 && att.pageMatch === 1,
        'bound=' + att.bound + '/' + att.total + ' pageMatch=' + att.pageMatch);

    // ---- G4 / G5 开 → 关（一次完整流程）----
    matchLog = []; clicks = []; launches = 0;
    await app2.launch({ exe: 'PSU.exe', title: /PSU/ });
    await app2.sync({ settle: 1 });
    await app2.at('/main/btnOutput').click();            // 打开电源
    await app2.expect('/main/lblOutput', 'ON');          // 校验：读布局文本
    await app2.at('/main/btnOutput').click();            // 关闭电源
    await app2.expect('/main/lblOutput', 'OFF');
    const pixelKinds = matchLog.filter((l) => l.kind !== 'layout');
    gate('G4 不涉像素：寻址全部走 layout',
        pixelKinds.length === 0 && matchLog.length >= 2,
        'kinds=' + JSON.stringify(matchLog.map((l) => l.kind)));
    gate('G5 开/关电源：点击序列与断言都对',
        launches === 1 && clicks.join('>') === '/main/btnOutput>/main/btnOutput' && scene.out === 'OFF',
        'clicks=' + clicks.join('>') + ' out=' + scene.out);

    // ---- G6 偶发(A)：第 1 次找不到 ⇒ transient（本地重试即可）----
    scene = { out: 'OFF', hideBtn: true }; live = layoutOf(scene);
    let dA: Diagnosis | null = null;
    try { await app2.at('/main/btnOutput').click(); } catch (e) { dA = await failing(e, app2, '/main/btnOutput', 1); }
    gate('G6 偶发(A)：第 1 次找不到判 transient（重试即可，不问模型）',
        !!dA && dA.kind === 'transient' && dA.transient && dA.action === 'wait',
        dA ? formatDiagnosis(dA) : '没有拿到诊断');
    // 重试后界面就绪 ⇒ 真的能点（"偶发"的含义就是重试能好）
    scene = { out: 'OFF' }; live = layoutOf(scene);
    await app2.at('/main/btnOutput').click();
    gate('G6b 偶发(A)：重试后成功（不需要模型）', scene.out === 'ON', 'out=' + scene.out);

    // ---- G7 真错·换页(D)：稳定复现 + 指纹对不上 ----
    scene = { out: 'OFF', otherApp: true }; live = layoutOf(scene);
    let dD: Diagnosis | null = null;
    try { await app2.at('/main/btnOutput').click(); } catch (e) { dD = await failing(e, app2, '/main/btnOutput', 3); }
    gate('G7 真错(D)：稳定复现 + 指纹对不上 ⇒ page-miss / ask-model',
        !!dD && dD.kind === 'page-miss' && !dD.transient && dD.action === 'ask-model',
        dD ? formatDiagnosis(dD) : '没有拿到诊断');

    // ---- G8 真错·漂移(E)：同页但路径变了 ----
    scene = { out: 'OFF', renameBtn: true }; live = layoutOf(scene);
    let dE: Diagnosis | null = null;
    try { await app2.at('/main/btnOutput').click(); } catch (e) { dE = await failing(e, app2, '/main/btnOutput', 3); }
    gate('G8 真错(E)：同页里路径变了 ⇒ path-drift / ask-model',
        !!dE && dE.kind === 'path-drift' && dE.action === 'ask-model' && !dE.evidence.fingerprintChanged,
        dE ? formatDiagnosis(dE) : '没有拿到诊断');

    // ---- G9 前置缺失(B)：节点在但禁用 ----
    scene = { out: 'OFF', disableBtn: true }; live = layoutOf(scene);
    let dB1: Diagnosis | null = null;
    let dB3: Diagnosis | null = null;
    try { await app2.expect('/main/btnOutput', { enabled: true }); }
    catch (e) {
        dB1 = await failing(e, app2, '/main/btnOutput', 1, { enabled: true });
        dB3 = await failing(e, app2, '/main/btnOutput', 3, { enabled: true });
    }
    gate('G9 前置缺失(B)：稳定复现 ⇒ precondition / add-precondition',
        !!dB3 && dB3.kind === 'precondition' && dB3.action === 'add-precondition',
        dB3 ? formatDiagnosis(dB3) : '没有拿到诊断');
    gate('G9b 偶发优先：同样的问题第 1 次先判 transient',
        !!dB1 && dB1.transient, dB1 ? formatDiagnosis(dB1) : '没有拿到诊断');

    // ---- G10 弹窗(C)：该在的都在 + 多出一个顶层节点 ----
    // （⚠ 弹窗判据要能区分"多出来的对话框"和"改了名的控件"：所以这里**不能**让按钮消失 ——
    //   按钮消失属 path-drift。真实形状是"点到了但被挡住、状态没变"。）
    scene = { out: 'OFF', popup: true }; live = layoutOf(scene);
    let dC: Diagnosis | null = null;
    try {
        await app2.at('/main/btnOutput').click();            // 点到了，但被对话框挡着
        await app2.expect('/main/lblOutput', 'ON');          // 状态没变 ⇒ 报错
    } catch (e) { dC = await failing(e, app2, '/main/lblOutput', 1, 'ON'); }
    gate('G10 弹窗(C)：多出顶层节点 ⇒ popup / dismiss-popup',
        !!dC && dC.kind === 'popup' && dC.action === 'dismiss-popup',
        dC ? formatDiagnosis(dC) + ' extras=' + JSON.stringify(dC.evidence.extraPaths) : '没有拿到诊断');

    // ---- G11 无布局：整树显式退化 ----
    live = null;
    const app3: any = App.build(PERCEPTION, { layout: null });
    const att3 = await app3.attach(null);
    gate('G11 无布局：整树显式标 degraded（不静默兜底）',
        att3.degraded.length === att3.total && att3.bound === 0,
        'degraded=' + att3.degraded.length + '/' + att3.total);
    matchLog = [];
    const btn3: any = app3.find('button', '输出开关');
    let rose = false;
    try { await btn3.click(); } catch (e) { rose = true; }
    gate('G11b 无布局：退到几何兜底，不崩、也不假装有布局',
        !rose && matchLog.filter((l) => l.kind === 'layout').length === 0,
        'locators=' + JSON.stringify(matchLog.map((l) => l.kind)));

    // ---- 汇总 ----
    const bad = gates.filter((g) => !g.ok);
    console.log('[layout_smoke] 硬门 ' + (gates.length - bad.length) + '/' + gates.length + ' 通过');
    if (bad.length) throw new Error('[layout_smoke] FAIL: ' + bad.map((b) => b.name).join(' | '));
    console.log('[layout_smoke] PASS');
}

main().catch((e: any) => {
    console.error('[layout_smoke] 异常: ' + (e && e.message ? e.message : e));
    throw e;      // 让宿主以非零退出（对齐 jsapi_example"check 失败即红"的口径）
});
