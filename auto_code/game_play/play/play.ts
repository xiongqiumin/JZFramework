// ============================================================================
// play.ts —— **一键通关脚本**（本任务的最终产出；跑在 virtual_device 容器里）
// ============================================================================
// 形状（脚本就该长这样 —— 它就是"一台小电脑上的自动化脚本"）：
//
//     active_window(gba);              // 点一下把游戏窗口激活（CU 的单击会命中并激活窗口）
//     加载存档();                       // ⚠ 还没有存档接口，见下面 §存档
//     while (!玩完) {
//         screentCaputer();            // 截图（同时落盘 = 给外部观测的"界面"）
//         dosomeThing();               // 决策 → key()
//     }
//
// 运行时**大模型不参与**：脚本自己按逻辑跑完一局。大模型在**局与局之间**干活 ——
// 看 play.log + frames/（脚本自己留的）与驱动抓的外部帧，然后回来改这个文件。
//
// 能用什么（就这些；红线见 prompt.md）：
//   · `__jz.computeruse.*` —— screenshot / click / key / wait / layoutTree / match / ocr*
//     （**只看得见画面、只操作得了鼠标键盘**，看不见游戏内部状态）
//   · `fs` / `path`（写日志与截图）、计时器、`console`
//
// 参数从运行根（= 本 js 所在目录，即 build/play）的 play.json 读，由驱动（run.py）写：
//   { out, window, warmupSec, steps, stepMs, maxSeconds }
//
// ----------------------------------------------------------------------------
// §存档（还没做）
// ----------------------------------------------------------------------------
// 这一版没有"读/写模拟器存档"的接口（JZEmulator 只暴露 openRom/start/stop/frameCount），
// 所以每局都从**开机 logo**开始。要"从第 3 章继续调"得先给容器/模拟器加存档接口 ——
// 那属于框架的活，不是这个脚本自己刷得出来的。
// ============================================================================

import fs from 'fs';
import { Buffer } from 'buffer';

interface PlayCfg {
    out: string;         // 产物目录（绝对路径）：play.log / frames/ / session.json 都写这儿
    app: string;         // 目标 App 的**程序名**（容器里那个游戏机进程，驱动装好的：gba）
    window: string;      // 目标窗口标题（游戏机进程建的那个窗口）
    warmupSec: number;   // 激活窗口后先等多久（过开机 logo）
    steps: number;       // 最多多少拍（0 = 不看拍数）
    stepMs: number;      // 每拍间隔（毫秒）
    maxSeconds: number;  // 最多跑多少秒（0 = 不看时间）
}

// ---- GBA 键位：**脚本是"拿着键盘的那个人"**，所以映射写在这里（左边是游戏机上印的字）----
// 权威映射在 JZEmulatorView::gbaKeyOf（键盘 -> GBA）；这里必须与它一致，否则白按。
const KEY: { [k: string]: string } = {
    A: 'Z', B: 'X', Start: 'Return', Select: 'Shift',
    L: 'A', R: 'S',
    Up: 'Up', Down: 'Down', Left: 'Left', Right: 'Right',
};

const dir = jzApp.applicationDirPath();
const cfg = JSON.parse(fs.readFileSync(dir + '/play.json', 'utf8')) as PlayCfg;
const cu = __jz.computeruse;

// ⚠ 路径一律**自己拼正斜杠**，别用 JS 的 path 模块：容器里的 path 是 POSIX 口径，
//   `path.join('E:/x','frames')` 会给出 `/E:/x/frames`（多一个前导斜杠）⇒ mkdirSync EACCES。
const OUT = String(cfg.out).replace(/\\/g, '/').replace(/\/+$/, '');
const joinPath = (a: string, b: string): string => a + '/' + b;
const framesDir = joinPath(OUT, 'frames');
fs.mkdirSync(framesDir, { recursive: true });
const logPath = joinPath(OUT, 'play.log');

const t0 = Date.now();
const state = { steps: 0, keys: [] as string[], saved: [] as string[], errors: [] as string[] };

function log(line: string): void {
    const s = '[' + ((Date.now() - t0) / 1000).toFixed(1) + 's] ' + line;
    console.log(s);
    fs.appendFileSync(logPath, s + '\n');
}
function sleep(ms: number): void { if (ms > 0) cu.wait(ms); }

/** 截图 → PNG 落盘（观测面），并把 dataURL 原样返回（用来判"画面变没变"） */
function shot(tag: string): { file: string; data: string } {
    const data: string = cu.screenshot();
    const file = joinPath(framesDir, tag + '.png');
    const b64 = data.indexOf(',') >= 0 ? data.slice(data.indexOf(',') + 1) : data;
    try {
        fs.writeFileSync(file, Buffer.from(b64, 'base64'));
        state.saved.push(tag + '.png');
    } catch (e) {
        state.errors.push('写帧失败: ' + e);
    }
    return { file, data };
}

/** 按一下（GBA 键名）。CU 的 key() 是"按下+松开"一瞬间 ⇒ 游戏里算"点一下"。 */
function tap(name: string, times = 1): void {
    const win = KEY[name];
    if (!win) { state.errors.push('不认识的键: ' + name); return; }
    for (let i = 0; i < times; i++) {
        cu.key(win);
        state.keys.push(name);
    }
}

/**
 * 按住（毫秒）。⚠ CU 没有"按住"原语，而 GBA 的方向键必须**跨帧**一直被按下才算走 ——
 * 所以这里按帧重复按：每 ~16ms 补一次 key()，中间不能省（一次 key() 只顶一帧）。
 */
function hold(name: string, ms: number): void {
    const win = KEY[name];
    if (!win) { state.errors.push('不认识的键: ' + name); return; }
    const end = Date.now() + ms;
    while (Date.now() < end) {
        cu.key(win);
        state.keys.push(name);
        cu.wait(16);
    }
}

/** 目标窗口在不在（用 layoutTree 探；找不到窗口它返回 null）—— 也可当"激活"用 */
function windowReady(title: string): boolean {
    try {
        const raw: any = cu.layoutTree(JSON.stringify({ title }));
        if (!raw) return false;
        return typeof raw === 'string' ? raw.length > 2 : true;
    } catch (e) {
        return false;
    }
}

function waitWindow(title: string, timeoutMs: number): boolean {
    const end = Date.now() + timeoutMs;
    while (Date.now() < end) {
        if (windowReady(title)) return true;
        sleep(300);
    }
    return false;
}

// ============================================================================
// 策略 —— **这就是要改的地方**（默认这份是"能跑起来的骨架"，不是会玩游戏的脚本）
// ============================================================================
/**
 * 一拍一决策。返回这一步要做什么。
 * 现在能用的信号只有两样：① 画面**变没变**（截图字节是否相同）；② 自己按过什么。
 * "看懂画面"（认标题页/章节/战斗）得靠：拿到帧 → 离线看/算 → 把结论写进这里
 * （脚本运行时不让大模型参与，所以判断必须是脚本自己能算的）。
 */
function strategy(step: number, changed: boolean): { press: string[]; hold?: string; holdMs?: number; note: string } {
    if (step === 0)
        return { press: ['Start'], note: '标题页：按 Start 进菜单（实测这一步会换屏）' };
    if (!changed)
        return { press: ['A'], note: '画面没动 → 按 A 试探（多半是"确认/继续"）' };
    return { press: [], note: '画面在自己动（过场/动画）→ 等它跑完' };
}

// ============================================================================
// 主循环
// ============================================================================
function finish(reason: string): void {
    const session = {
        script: 'play.ts',
        reason,
        steps: state.steps,
        elapsedMs: Date.now() - t0,
        keys: state.keys,
        frames: state.saved,
        errors: state.errors,
        window: cfg.window,
        cfg,
        ts: Date.now(),
    };
    try {
        fs.writeFileSync(joinPath(OUT, 'session.json'),
            JSON.stringify(session, null, 2) + '\n', 'utf8');
    } catch (e) {
        console.log('[play] 写 session.json 失败: ' + e);
    }
    log('[play] 收工: reason=' + reason + ' steps=' + state.steps
        + ' 按键 ' + state.keys.length + ' 次 帧 ' + state.saved.length + ' 张'
        + (state.errors.length ? (' 错误 ' + state.errors.length + ' 条') : ''));
}

function main(): void {
    // 容器里就两个进程：cfg.app（游戏机，题目侧装好的）+ 本脚本自己。这里只"认窗口"
    // —— 程序是驱动装起来的（脚本没有起进程的权限，虚拟桌面下 launch 也不启动真进程）。
    log('[play] 目标 App: 进程 "' + (cfg.app || 'gba') + '" / 窗口 "' + cfg.window
        + '"，产物目录: ' + OUT);
    if (!waitWindow(cfg.window, 15000)) {
        state.errors.push('等不到窗口: ' + cfg.window);
        finish('no-window');
        return;
    }
    log('[play] 窗口在。点一下把它激活（之后按键才会落到游戏上）');
    // ⚠ CU 的绑定是**全参**：click(x, y, btn) 三个都得给（少了报 "需要3个参数"）。
    cu.click(0.5, 0.5, 'left');   // 归一化坐标：整块桌面的正中（游戏窗口就在桌面中间）
    sleep(400);

    if (cfg.warmupSec > 0) {
        log('[play] 预热 ' + cfg.warmupSec + 's（让游戏跑过开机 logo）');
        sleep(cfg.warmupSec * 1000);
    }

    let prev = '';
    let reason = 'steps';
    for (let step = 0; ; step++) {
        if (cfg.steps > 0 && step >= cfg.steps) { reason = 'steps'; break; }
        if (cfg.maxSeconds > 0 && (Date.now() - t0) / 1000 >= cfg.maxSeconds) { reason = 'time'; break; }

        const s = shot('f' + String(step).padStart(5, '0'));
        const changed = prev !== '' && s.data !== prev;
        prev = s.data;

        const act = strategy(step, changed);
        if (act.hold)
            hold(act.hold, act.holdMs || 200);
        for (const k of act.press)
            tap(k);

        log('step ' + step + ' ' + framesDirName(s.file) + ' 画面变没变=' + (changed ? '变了' : '没变')
            + ' 按=' + ((act.hold ? ['按住' + act.hold] : []).concat(act.press).join('+') || '(不动)')
            + ' | ' + act.note);

        state.steps++;
        sleep(cfg.stepMs);
    }
    finish(reason);
}

function framesDirName(file: string): string {
    const i = file.lastIndexOf('\\') >= 0 ? file.lastIndexOf('\\') : file.lastIndexOf('/');
    return i >= 0 ? file.slice(i + 1) : file;
}

// ⚠ 崩了也要留证据：异常照样写 session.json（reason=crash）——
// 否则外面只能看到"脚本不见了"，分不清是它写错了还是环境坏了。
try {
    main();
} catch (e) {
    state.errors.push('脚本异常: ' + e);
    log('[play] !! 脚本异常: ' + e);
    finish('crash');
}
