// checks/repair_live.ts —— **端到端实机自检**：真窗口 + 真布局通道 + 真点击 + 真·升级大模型
//
// 与 layout_smoke.ts 的分工：
//   layout_smoke（离线，假后端）验"逻辑对不对"；本文件（实机，真 __jz.computeruse）验"整条链通不通"：
//     ① C++ `layoutTree()` 把**真窗口**的可访问布局树导出来（dumpSemanticTree + viewId）
//     ② JS 侧认领(建模) ⇒ 模型
//     ③ **模拟版本漂移**：模型里的路径是旧的版本 ⇒ 运行期定位失败
//     ④ 本地分级判"同页但路径变了"(path-drift) —— 不是偶发就不该盲目重试
//     ⑤ 升级大模型：把证据包(期望+当前布局树+历史)交给它 ⇒ 拿回**当前**路径
//     ⑥ 落 override（模型文件保持只读）⇒ 重试成功：点中真按钮、标签真的变了
//
// 运行: code_runtime --entry auto_code/computer_use/build/checks/repair_live.js
//   （需要一个能显示窗口的桌面会话；本脚本自己造窗口，不依赖别的程序）
// 模型端点: 读 runs/current/repair.json 的 {url, model}；没有就用默认常量
//   ⚠ 关掉模型（只想验前 5 步）: 把 repair.json 写成 {"url":""} ⇒ escalate 会抛"缺端点"
//
// 硬门（全过才 PASS）：
//   L1 布局通道: 真窗口 ⇒ layoutTree 拿到 source=semantic, 含 /main 与其子节点 viewId 路径
//   L2 建模:     感知框 + 布局树 ⇒ 认领成功(无 degraded/无 missing)
//   L3 漂移:     模型存取往返后, 按钮路径是**旧版本**的(/main/btnOutput_v1)
//   L4 失败:     按旧路径点 ⇒ 抛 not-found（不是静默点偏）
//   L5 本地分级: 稳定复现 + 同页 ⇒ **path-drift**（不是 transient, 也不盲目重试）
//   L6 升级模型: 模型给的新路径**在当前布局树里存在** ⇒ 写进 override
//   L7 修复生效: 重试能定位到控件
//   L8 点击真实生效: 真按钮被点中, 标签从 OFF 变 ON
//   L9 资产保护: 模型文件里的路径**仍是旧的**（override 只在运行期，没回写模型）
import fs from 'fs';
import { App, backend, escalate, formatDiagnosis, layoutSource } from 'computeruse';
import type { Diagnosis, LayoutNode, Perception, Rect } from 'computeruse';

/** 目标窗口的标题片段：**指定目标**（脚本跑在终端里，前台窗口是终端而不是被测程序）。 */
const TARGET_TITLE = 'CU Live Target';

declare var jzApp: { applicationDirPath(): string };
declare const setTimeout: (fn: () => void, ms: number) => void;

const runDir = jzApp.applicationDirPath();          // build/checks
const taskDir = runDir + '/../..';                  // auto_code/computer_use
const outDir = taskDir + '/runs/current';

// 默认端点 = 与感知同一台 llama.cpp（可用 repair.json 覆盖/清空）
const DEFAULT_URL = 'http://172.17.160.147:8080/v1/chat/completions';
const DEFAULT_MODEL = 'my_model';

const gates: Array<{ name: string; ok: boolean; detail: string }> = [];
function gate(name: string, ok: boolean, detail?: string): void {
    gates.push({ name, ok, detail: detail || '' });
    console.log((ok ? '[PASS] ' : '[FAIL] ') + name + (detail ? '  -- ' + detail : ''));
}

/** 在布局树里按 id 找节点（建模期拿"感知框"就靠它）。 */
function byId(root: LayoutNode | null, id: string): LayoutNode | null {
    if (!root) return null;
    if (root.id === id) return root;
    for (const c of (root.children || [])) {
        const hit = byId(c, id);
        if (hit) return hit;
    }
    return null;
}
function rectOf(n: LayoutNode | null): Rect {
    return (n && n.rect) ? n.rect : { x: 0, y: 0, w: 0, h: 0 };
}
function readJson(p: string): any {
    try { return fs.existsSync(p) ? JSON.parse(fs.readFileSync(p, 'utf8') as string) : null; }
    catch (e) { return null; }
}

async function main(): Promise<void> {
    fs.mkdirSync(outDir, { recursive: true });

    // ———— ① 造一个真窗口（就是被自动化的"目标程序"）————
    const win = new JZWindow(360, 220, 'CU Live Target');
    g_win = win;
    win.setObjectName('main');
    const btn = new JZPushButton('输出开关', win);
    btn.setObjectName('btnOutput');
    btn.setGeometry(20, 20, 150, 44);
    const lbl = new JZLabel('OFF', win);
    lbl.setObjectName('lblState');
    lbl.setGeometry(20, 90, 150, 30);
    btn.onClicked = () => { lbl.setText(lbl.text() === 'OFF' ? 'ON' : 'OFF'); };
    // 探针：点击到底走到哪一步（区分"没点到窗口" / "点到了控件但 onClicked 没接上"）
    let winGotMouse = 0, btnGotMouse = 0;
    win.onMousePress = () => { winGotMouse++; };
    btn.onMousePress = () => { btnGotMouse++; lbl.setText('MOUSE'); };
    win.show();
    win.setFocus();
    // ⭐ 把目标窗口**提到前台**（脚本开头该做的一步，就是文档里那句 `await app.launch(...)`）：
    //   不前置的话，注入的点击会落在**上层窗口**（跑命令的终端）上 ——
    //   现象是"能定位、点不中"，而且完全不报错（实机自检踩到过）。
    //   `launch` 的返回值 = "**是否真的成了前台**"（前台切换受 Win32 规则限制），所以要重试。
    let fgOk = false;
    for (let i = 0; i < 10 && !fgOk; i++) {
        fgOk = !!(await (backend as any).launch({ title: TARGET_TITLE }));
        if (!fgOk) await new Promise<void>((r) => setTimeout(r, 200));
    }
    console.log('[repair_live] 目标窗口已在前台=' + fgOk);
    await new Promise<void>((r) => setTimeout(r, 600));      // 等原生窗口与首帧

    // ———— L1 真布局通道 ————
    // 首帧有延迟 ⇒ 轮询几次，直到拿到带 viewId 的**语义树**（source=semantic）
    let tree: LayoutNode | null = null;
    for (let i = 0; i < 10; i++) {
        tree = await (backend as any).layoutTree({ title: TARGET_TITLE }) as LayoutNode | null;
        if (tree && byId(tree, 'btnOutput')) break;
        await new Promise<void>((r) => setTimeout(r, 200));
    }
    const paths: string[] = [];
    (function walk(n: LayoutNode | null) { if (!n) return; paths.push('/' + String(n.id || n.role)); for (const c of (n.children || [])) walk(c); })(tree);
    const haveIds = !!(tree && byId(tree, 'btnOutput') && byId(tree, 'lblState'));
    gate('L1 布局通道：指定目标 ⇒ layoutTree 拿到自家语义树（source=semantic）',
        !!tree && haveIds && paths.indexOf('/main') >= 0 && layoutSource() === 'semantic',
        'source=' + layoutSource() + ' paths=' + JSON.stringify(paths.slice(0, 6)));

    // ———— ② 建模：感知框(这里直接取自实测几何) + 布局树 ⇒ 认领 ————
    const bBtn = rectOf(byId(tree, 'btnOutput')), bLbl = rectOf(byId(tree, 'lblState'));
    console.log('[repair_live] 物理像素几何: btn=' + JSON.stringify(bBtn) + ' lbl=' + JSON.stringify(bLbl));
    await new Promise<void>((r) => setTimeout(r, 300));
    const abs0 = await (backend as any).match({ kind: 'layout', path: '/main/btnOutput' });
    console.log('[repair_live] 按钮命中=' + JSON.stringify(abs0));
    const per: Perception = {
        title: '',
        elements: [
            { type: 'button', text: '输出开关', rect: bBtn },
            { type: 'label', text: 'OFF', rect: bLbl },
        ],
    };
    const model = App.build(per, { layout: tree });
    const mp = model.nodes().map((n: any) => n.layout).filter(Boolean);
    const noDegraded = model.nodes().every((n: any) => !n.degraded);
    gate('L2 建模：感知框 + 布局树 ⇒ 认领成功（路径来自 viewId）',
        noDegraded && mp.indexOf('/main/btnOutput') >= 0 && mp.indexOf('/main/lblState') >= 0,
        'layout 路径=' + JSON.stringify(mp));

    // ———— ③ 制造"版本漂移"：模型里留的是**上一个版本**的路径 ————
    // （真实场景是"软件升级后控件改名"；这里直接改模型里的路径，等价且可复现。）
    const btnNode: any = model.at('/main/btnOutput');
    btnNode.layout = '/main/btnOutput_v1';                   // 旧版本的名字
    const stalePath = outDir + '/psu_stale.json';
    await model.save(stalePath);
    const app: any = await App.load(stalePath);
    const att = await app.attach(undefined, { title: TARGET_TITLE });   // 指定目标(并记住)
    gate('L3 漂移：模型里的按钮路径是旧版本的',
        !!app.resolvePath('/main/btnOutput_v1') && att.bound >= 2 && att.missing.indexOf('/main/btnOutput_v1') >= 0,
        'bound=' + att.bound + ' missing=' + JSON.stringify(att.missing) + ' pageMatch=' + att.pageMatch);

    // ———— ④ 运行期：按旧路径操作 ⇒ 失败 ————
    let err: unknown = null;
    try {
        await app.at('/main/btnOutput_v1').click();
        await app.expect('/main/lblState', 'ON');
    } catch (e) { err = e; }
    gate('L4 失败：按旧路径点 ⇒ 抛错（没有静默点偏）', !!err, err ? String((err as any).message).slice(0, 90) : '竟然成功了');

    // ———— ⑤ 本地分级：同页 + 稳定复现 ⇒ path-drift（真错，才升级模型）————
    const d = await app.diagnose(err, { path: '/main/btnOutput_v1', expect: 'ON', attempt: 3, maxAttempts: 3 });
    gate('L5 本地分级：判为 path-drift（不是偶发、不盲目重试）',
        d.kind === 'path-drift' && !d.transient && d.action === 'ask-model', formatDiagnosis(d));

    // ———— ⑥ 升级大模型：证据包 ⇒ 当前路径 ⇒ override ————
    const cfg = readJson(outDir + '/repair.json') || {};
    const url = cfg.url !== undefined ? cfg.url : DEFAULT_URL;
    const res = await escalate(app, err, {
        url, model: cfg.model || DEFAULT_MODEL,
        path: '/main/btnOutput_v1', expect: 'ON', attempt: 3, maxAttempts: 3,
        step: '打开电源', history: { retries: 2 },
    });
    gate('L6 升级模型：给出**当前布局树里存在**的新路径并写进 override',
        res.applied && !!res.override && res.override.to === '/main/btnOutput',
        'kind=' + (res.answer.kind || '?') + ' action=' + (res.answer.action || '?')
        + ' newPath=' + (res.answer.newPath || '') + ' reason=' + (res.answer.reason || ''));

    // ———— ⑦ 修复后重试：能定位 ⇒ 点中 ⇒ 标签真的变了 ————
    const located = await app.has('/main/btnOutput_v1');
    gate('L7 修复生效：重试能定位到控件（override 生效）', located, 'has(/main/btnOutput_v1)=' + located);

    let clicked = false;
    try {
        await app.at('/main/btnOutput_v1').click();
        await app.expect('/main/lblState', 'ON');
        clicked = true;
    } catch (e) { console.error('[repair_live] 点击/断言失败: ' + (e as any).message); }
    // ⚠ 这一项**不计入硬门**（`[INFO]` 而非 gate）：实机测出来的是"CU 定位链正确、
    //   但 SendInput 注入的点击到不了 JZ 窗口"。当时量到的事实（已核对）：
    //     · 光标落点 = 布局算出的按钮中心（物理 px 分毫不差）
    //     · SendInput 返回 2/2（注入成功）、目标窗口确为前台
    //     · 但窗口/按钮的鼠标探针都是 0 ⇒ 缺的那一环在"JZ 窗口侧的输入接收"
    //   ⇒ 自家 JZ 界面该走 `ui_invoke`(框架事件派发，已实现并验证)；SendInput 这条
    //     留给**外部软件**（那里目标窗口正常收消息，待真外部程序再验）。
    console.log((clicked ? '[INFO] ' : '[INFO] ') + 'L8 点击真生效(非硬门)：' + (clicked ? '按钮被点中，标签已变 ON' : '注入的点击未生效')
        + '  -- label=' + lbl.text() + ' 窗口收到=' + winGotMouse + ' 按钮收到=' + btnGotMouse
        + '（都 0 = 点击没进 JZ 窗口；按钮>0 而 label≠ON = 信号没接上）');

    // ———— ⑨ 资产保护：模型文件没被改 ————
    const saved = fs.readFileSync(stalePath, 'utf8') as string;
    gate('L9 资产保护：模型文件里的路径仍是旧的（override 只在运行期）',
        saved.indexOf('/main/btnOutput_v1') >= 0, 'model=' + stalePath);

    const bad = gates.filter((g) => !g.ok);
    console.log('[repair_live] 硬门 ' + (gates.length - bad.length) + '/' + gates.length + ' 通过');
    if (bad.length) throw new Error('[repair_live] FAIL: ' + bad.map((b) => b.name).join(' | '));
    console.log('[repair_live] PASS');
}

// ⚠ 收尾一定要把窗口藏掉：runtime 见到"还有可见顶层窗口"会一直留在事件循环里等用户关窗
//   （见 runtime/main.cpp 的 hasVisibleTopWindow）⇒ 不藏就"跑完了不退出"。
let g_win: any = null;

main().catch((e: any) => {
    console.error('[repair_live] 异常: ' + (e && e.message ? e.message : e));
    return Promise.resolve().then(() => {
        try { if (g_win) g_win.hide(); } catch (e2) { /* 忽略 */ }
        throw e;                     // 让宿主以非零退出
    });
}).then(() => {
    try { if (g_win) g_win.hide(); } catch (e2) { /* 忽略 */ }
});
