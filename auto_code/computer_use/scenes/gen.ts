// scenes/gen.ts —— 场景生成器（= 本题的"题目"）
//
// ⭐ 一份声明 → 两条出口（同源，绝不各写一遍）：
//    ① 声明 → uiLoader XML → 真控件树（屏幕上真能看到、能截图、能被点击）
//    ② 同一份声明 → scenes/truth.json（判分用真值）
//
// 运行：
//   1) 编译： <tsc> -p auto_code/computer_use/tsconfig.json
//   2) 生成： code_runtime --debug --entry <task>/build/scenes/gen.js
//      种子/难度从"运行根（build/scenes）/gen.json"读（由 run.py 写入）；不存在则用默认值。
// 注意：脚本建了可见窗口 ⇒ 必须自己 jzApp.quit()，否则 code_runtime 不会退出。

import fs from 'fs';

type Rect = [number, number, number, number];

interface Node {
    id: string;
    type: string;          // 控件词汇（与 computeruse 包的 type 对齐）
    cls: string;           // uiLoader 的短类型名（见 JZJSUiLoader::uiClassMap）
    text: string;
    rect: Rect;
    parent: string | null;
}

interface Scene {
    case: string;
    seed: number;
    level: number;
    title: string;
    size: [number, number];
    nodes: Node[];
}

// —— 随机（mulberry32：同种子必同结果，评测要可复现）——
function makeRng(seed: number): () => number {
    let a = (seed >>> 0) + 0x6d2b79f5;
    return () => {
        a |= 0; a = (a + 0x6d2b79f5) | 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

const WORDS_BUTTON = ['确定', '取消', '读取', '写入', '刷新', '连接', '断开', '清空'];
const UNITS = ['V', 'A', '℃', 'ms'];

let seq = 0;
function nid(): string { return 'n' + (seq++); }

function node(type: string, cls: string, text: string, rect: Rect, parent: string | null): Node {
    return { id: nid(), type, cls, text, rect, parent };
}

// —— L0：固定的小界面（手写基准，用来校准尺子与流程）——
function sceneL0(seed: number): Scene {
    seq = 0;
    const W = 420, H = 320;
    // ⚠ 根节点的 text 一律空串：窗口标题画在**非客户区**(标题栏)，而截图只含客户区
    //   （见 JZScreen::grabWindow）⇒ 按契约"看不到文字就留空串"。真实标题仍由 scene.title 设进窗口。
    const root = node('window', 'Window', '', [0, 0, W, H], null);
    const nodes: Node[] = [root];
    nodes.push(node('edit', 'LineEdit', '0', [12, 40, 396, 40], root.id));
    const btns: Array<[string, number]> = [['7', 12], ['8', 114], ['9', 216], ['+', 318]];
    for (const [t, x] of btns) nodes.push(node('button', 'PushButton', t, [x, 100, 92, 44], root.id));
    return { case: 'l0_plain', seed, level: 0, title: '计算器', size: [W, H], nodes };
}

// —— L1：随机布局（位置/尺寸/文本/数量都随种子变；网格排布保证互不重叠）——
// ⭐ 只收 **真值框 = 屏幕上可见的外接框** 的控件（判分按 IoU 比框，感知只能看像素）。
//    - `PushButton` / `LineEdit` 自带完整边框 ⇒ 可见框 == 控件 geometry（格子给多大就多大）✓
//    - `CheckBox` / `RadioButton` **没有整体边框** ⇒ 可见框只能算"指示器 + 文字"的墨迹框：
//      24px 高的框上左右/上下各差 3px，IoU 就掉到 0.5~0.7（贴着"算不算命中"的 0.5 边界），
//      属模糊判定 ⇒ **暂不进题目**。要收它们，先给它们一个可见边框（`JZCheckbox::paintEvent`
//      目前不画样式盒子 drawBox，Label/PushButton 画；或定义"墨迹框"口径）。
//    - `Label` 同理（连宽度都随字数变）⇒ 不进题目。
//    结论：本题当前只判"有边框的控件"；加难度时优先补**带边框的容器**（GroupBox/TabWidget），
//    那才顺带把 hierF1 变成真指标（现在所有节点都直接挂窗口）。
function sceneL1(seed: number): Scene {
    seq = 0;
    const rng = makeRng(seed);
    const cols = 3, rows = 3;
    const cellW = 150, cellH = 60, pad = 12, top = 44;
    const W = pad * 2 + cols * cellW, H = top + rows * cellH + pad;

    const root = node('window', 'Window', '', [0, 0, W, H], null);   // 根节点文本留空，理由见 sceneL0
    const nodes: Node[] = [root];

    const kinds = ['button', 'button', 'edit'];
    const count = 4 + Math.floor(rng() * 5);            // 4~8 个
    for (let k = 0; k < count; k++) {
        const c = k % cols, r = Math.floor(k / cols);
        const x = pad + c * cellW, y = top + r * cellH;
        const kind = kinds[Math.floor(rng() * kinds.length)];
        const pick = (arr: string[]) => arr[Math.floor(rng() * arr.length)];
        if (kind === 'button') {
            nodes.push(node('button', 'PushButton', pick(WORDS_BUTTON), [x, y, 120, 40], root.id));
        } else if (kind === 'edit') {
            const v = String(Math.floor(rng() * 900) + 100) + pick(UNITS);
            nodes.push(node('edit', 'LineEdit', v, [x, y + 6, 120, 28], root.id));
        }
    }
    return { case: 'l1_random', seed, level: 1, title: '设备面板', size: [W, H], nodes };
}

function buildScene(seed: number, level: number): Scene {
    return level <= 0 ? sceneL0(seed) : sceneL1(seed);
}

// —— 声明 → uiLoader 的 Qt Designer 风格 XML ——
function toXml(s: Scene): string {
    const esc = (t: string) => t.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;');
    const out: string[] = [];
    out.push('<?xml version="1.0" encoding="UTF-8"?>');
    out.push('<ui version="4.0">');
    out.push('    <class>AutoCodeScene</class>');
    const kids = (parentId: string | null) => s.nodes.filter(n => n.parent === parentId);
    const emit = (n: Node, indent: string) => {
        out.push(indent + '<widget class="' + n.cls + '" name="' + n.id + '">');
        if (n.type === 'window') out.push(indent + '    <property name="title">' + esc(n.text) + '</property>');
        else if (n.text) out.push(indent + '    <property name="text">' + esc(n.text) + '</property>');
        out.push(indent + '    <property name="geometry">' + n.rect.join(',') + '</property>');
        for (const c of kids(n.id)) emit(c, indent + '    ');
        out.push(indent + '</widget>');
    };
    const root = kids(null)[0];
    emit(root, '    ');
    out.push('</ui>');
    return out.join('\n') + '\n';
}

function main(): void {
    // 运行根 = 本 js 所在目录（build/scenes）⇒ 任务根 = 上两级
    const runDir = jzApp.applicationDirPath();
    const taskDir = runDir + '/../..';
    let seed = 0, level = 0, caseName = '';
    let hold = true;          // 默认常驻：作为"假 App"进程给外部截图/点击（见 run.py）
    const cfgPath = runDir + '/gen.json';
    if (fs.existsSync(cfgPath)) {
        const cfg = JSON.parse(fs.readFileSync(cfgPath, 'utf8')) as any;
        if (typeof cfg.seed === 'number') seed = cfg.seed;
        if (typeof cfg.level === 'number') level = cfg.level;
        if (typeof cfg.case === 'string') caseName = cfg.case;
        if (typeof cfg.hold === 'boolean') hold = cfg.hold;
    }

    const scene = buildScene(seed, level);
    if (caseName) scene.case = caseName;

    // ① 真控件树（屏幕上真能看到）
    const xml = toXml(scene);
    const win = uiLoader.load(xml);
    win.setTitle(scene.title);
    win.show();

    // ② 同源导出真值
    const truth = {
        _comment: '本文件由 scenes/gen.ts 生成（题目真值）。人只读不改；改题目请改生成器。',
        case: scene.case, seed: scene.seed, level: scene.level, size: scene.size,
        nodes: scene.nodes.map(n => ({ id: n.id, type: n.type, text: n.text, rect: n.rect, parent: n.parent })),
    };
    fs.writeFileSync(taskDir + '/scenes/truth.json', JSON.stringify(truth, null, 2) + '\n', 'utf8');
    fs.writeFileSync(taskDir + '/runs/current/scene.xml', xml, 'utf8');

    console.log('[gen] case=' + scene.case + ' seed=' + scene.seed + ' level=' + scene.level
        + ' nodes=' + scene.nodes.length + ' size=' + scene.size.join('x'));
    console.log('[gen] truth -> scenes/truth.json ; xml -> runs/current/scene.xml');
    if (hold) console.log('[gen] holding (窗口常驻, 等外部截图/点击后由驱动结束进程)');
    else jzApp.quit();
}

main();
