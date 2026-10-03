// probe/explore.ts —— **固定探针入口**（评测的"被测程序"外壳）
//
// ⛔ 不要改这个文件来"绕过"实现：它是稳定入口，被测的东西在
//    `JZJSEngine/js_wrapper/computeruse`（`App.explore` 那条链路）。
//
// 职责（只做三件事）：
//   ① 把驱动准备好的"屏幕"（PNG）与窗口几何读进来 → 注入**离线后端**（setBackend）；
//   ② 调 `App.explore()`（被测实现）；
//   ③ 把结果树写成判分器的输入格式 `runs/current/tree.json`。
//
// 离线后端的边界（很重要）：
//   - 只有"设备原语"被替换掉：`screenshot()` 返回驱动给的 PNG（dataURL）；点击/按键/拖拽/等待是 no-op
//     （离线没有真桌面 —— 要真动作请走 ui_invoke 那条 MCP 通道，那是驱动的事）。
//   - `explore / fromPerception / saveModel / loadModel` **保持默认实现**（spread 自 `backend`）⇒
//     被测的正是 computeruse 包里那几个函数；探针不参与它们的逻辑。
//   - `match`（C++ 侧的 image/color 定位）在离线环境没有意义，仍是桩：恒返回 null。
//     `explore` 的"感知"走的是**感知后端**（`perceive()`，见 js_wrapper/computeruse/perceive.ts）：
//     它吃 dataURL 直接打多模态接口。要打哪台服务由驱动写进 probe.json 的 `vlm` 决定
//     （探针不内置地址）—— 换 OCR/检测后端只需要改 perceive.ts，本文件不用动。
//
// 运行: code_runtime --entry <task>/build/probe/explore.js
// 输入: <runDir>/probe.json（驱动写）; 缺省用 runs/current/shot.png + 无窗口信息
//    { "screenshot": "<路径: 相对任务根或绝对>", "window": [w,h], "shot": [w,h], "out": "<tree.json 路径>",
//      "vlm": { "url": "...", "model": "..." } }

import fs from 'fs';
import { App, setBackend, backend as defaultBackend } from 'computeruse';

// —— 运行根 = 本 js 所在目录（build/probe）⇒ 任务根 = 上两级（与 scenes/gen.ts 同一约定）——
const runDir = jzApp.applicationDirPath();
const taskDir = runDir + '/../..';
const curDir = taskDir + '/runs/current';

function absPath(p: string): string {
    if (!p) return '';
    if (p.indexOf(':') === 1 || p.startsWith('/')) return p;      // 绝对路径（Windows 盘符 / POSIX）
    return taskDir + '/' + p.replace(/^\.\//, '');
}

function readJson(p: string): any {
    try {
        if (!fs.existsSync(p)) return null;
        return JSON.parse(fs.readFileSync(p, 'utf8') as string);
    } catch (e: any) {
        console.error('[probe] 读不了 ' + p + ': ' + (e && e.message ? e.message : e));
        return null;
    }
}

function writeTree(path: string, doc: any): void {
    fs.writeFileSync(path, JSON.stringify(doc, null, 2) + '\n', 'utf8');
}

/** PNG 字节 → dataURL（引擎的 Uint8Array 有 toBase64；没有就退回全局 Buffer）。 */
function dataUrlOf(path: string): string {
    if (!fs.existsSync(path)) throw new Error('截图不存在: ' + path);
    const bytes: any = fs.readFileSync(path);
    const b64 = (bytes && typeof bytes.toBase64 === 'function')
        ? bytes.toBase64()
        : (globalThis as any).Buffer.from(bytes).toString('base64');
    return 'data:image/png;base64,' + b64;
}

// —— CU 节点（**物理像素** rect）→ 判分契约（同口径 rect + parent id）——
// ⚠ 2026-10-03 起 CU 的 rect 就是物理像素（与截图像素同一空间）⇒ 这里不再乘窗尺寸。
function toContract(app: any, win: [number, number]): any {
    const W = win[0] || 0, H = win[1] || 0;
    const nodes: any[] = [];
    let seq = 0;
    const rectOf = (n: any, isRoot: boolean): number[] => {
        const r = n && n.rect;
        if (!r) return isRoot ? [0, 0, W, H] : [0, 0, 0, 0];   // 根没给就按整窗
        return [Math.round(r.x), Math.round(r.y), Math.round(r.w), Math.round(r.h)];
    };
    const walk = (n: any, parentId: string | null): void => {
        const id = 'n' + (seq++);
        nodes.push({
            id,
            type: String(n.type || 'unknown'),
            text: typeof n.label === 'string' ? n.label : '',
            rect: rectOf(n, parentId === null),
            parent: parentId,
        });
        const kids: any[] = n.children || [];
        for (let i = 0; i < kids.length; i++) walk(kids[i], id);
    };
    if (app) walk(app, null);
    return { nodes };
}

/** 离线后端：只换设备原语，JS 侧实现（explore/fromPerception/...）原样保留。 */
function offlineBackend(dataUrl: string, stats: { matchCalls: number }): any {
    const def: any = defaultBackend;      // ⚠ 在 setBackend 之前取，才能拿到默认实现
    return {
        ...def,
        screenshot: async () => dataUrl,
        // ⚠ 感知桩：还没有后端能给出"屏幕上有哪些框/文字"（见文件头）。
        match: async () => { stats.matchCalls++; return null; },
        click: async () => true,
        drag: async () => true,
        key: async () => { },
        type: async () => { },
        wait: async () => { },
    };
}

function main(): void {
    const cfg = readJson(runDir + '/probe.json') || {};
    const shot = absPath(cfg.screenshot || 'runs/current/shot.png');
    const outPath = absPath(cfg.out || 'runs/current/tree.json');
    const win: [number, number] = Array.isArray(cfg.window) ? [cfg.window[0], cfg.window[1]] : [0, 0];
    const vlm = cfg.vlm && cfg.vlm.url ? cfg.vlm : null;      // 感知后端地址由驱动给（探针不内置）

    let dataUrl = '';
    try {
        dataUrl = dataUrlOf(shot);
    } catch (e: any) {
        console.error('[probe] ' + (e && e.message ? e.message : e));
    }

    const stats = { matchCalls: 0 };
    setBackend(offlineBackend(dataUrl, stats));

    if (!dataUrl) {
        // 连屏幕都没有：写空预测，让判分给出完整的"漏检"diff（这是预期的红灯，不是崩溃）
        writeTree(outPath, { nodes: [] });
        console.error('[probe] 没有截图 ⇒ 写空预测 -> ' + outPath);
        return;
    }

    const t0 = Date.now();
    Promise.resolve()
        .then(() => (App as any).explore({ name: 'probe', screenshot: { maxDim: 0 }, vlm }))
        .then((app: any) => {
            const doc = toContract(app, win);
            writeTree(outPath, doc);
            console.log('[probe] nodes=' + doc.nodes.length + ' shot=' + shot
                + ' win=' + win.join('x') + ' matchCalls=' + stats.matchCalls
                + ' vlm=' + (vlm ? (vlm.url + ' model=' + (vlm.model || 'default')) : '(未配置)')
                + ' elapsed=' + (Date.now() - t0) + 'ms');
            console.log('[probe] tree -> ' + outPath);
        })
        .catch((e: any) => {
            const msg = (e && e.message) ? e.message : String(e);
            writeTree(outPath, { nodes: [] });
            console.error('[probe] explore 失败: ' + msg);
            console.error('[probe] 已写空预测 -> ' + outPath + '（判分会给全漏检；这就是"下一步改哪里"）');
        });
}

main();
