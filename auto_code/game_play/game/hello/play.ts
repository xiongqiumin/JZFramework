// ============================================================================
// game/hello/play.ts —— hello 玩法的**参考脚本**（会真的玩：找红球 → 点它）
// ============================================================================
// 它是"这道题该怎么做"的示范，也是 `run_verify.py` 默认装的脚本（见那个文件的 --script）：
//
//     while (没跑完) {
//         截图 → 在画面上找**红球**（颜色 #e02020）→ 点它的中心
//         找不到就等下一拍（球每 4 秒换位置）
//     }
//
// 为什么**只找红色**：点蓝球要**扣分**（初始 5 分，掉到 0 就输），所以"看见什么点什么"是
// 错的 —— 只匹配红色，蓝球自然被忽略。这道题练的就是这个：**颜色 → 决策**。
//
// 能用什么：`__jz.computeruse.*`（screenshot / match / click / wait / layoutTree…）+ fs。
// ⚠ 红线（见 prompt.md）：只看得见画面、只操作鼠标键盘；不读游戏的状态文件、不碰 game/**。
// ============================================================================
import fs from 'fs';

interface PlayCfg {
    out: string;         // 产物目录（绝对路径）：play.log / frames/ / session.json 都写这儿
    app: string;         // 目标程序名（容器里那个游戏机进程）
    window: string;      // 目标窗口标题
    warmupSec: number;
    steps: number;       // 最多多少拍（0 = 不看拍数）
    stepMs: number;      // 每拍间隔（毫秒）
    maxSeconds: number;  // 最多跑多少秒（0 = 不看时间）
}

const cu = __jz.computeruse;
const dir = jzApp.applicationDirPath();
const cfg = JSON.parse(fs.readFileSync(dir + '/play.json', 'utf8')) as PlayCfg;

// ⚠ 路径自己拼正斜杠（容器里的 path 是 POSIX 口径，见 play/play.ts 的说明）
const OUT = String(cfg.out).replace(/\\/g, '/').replace(/\/+$/, '');
const framesDir = OUT + '/frames';
const logPath = OUT + '/play.log';
fs.mkdirSync(framesDir, { recursive: true });

const t0 = Date.now();
const state = { steps: 0, hits: 0, misses: 0, saved: 0, errors: [] as string[] };

function log(line: string): void {
    const s = '[' + ((Date.now() - t0) / 1000).toFixed(1) + 's] ' + line;
    console.log(s);
    fs.appendFileSync(logPath, s + '\n');
}
const sleep = (ms: number): void => { if (ms > 0) cu.wait(ms); };

// ---- 题目侧的颜色（与 game/hello/game.ts 的 kRed / kBlue 一致）----
const RED = '#e02020';
const BLUE = '#2050e0';

interface Box { x: number; y: number; w: number; h: number; }

/** 在**整块桌面**上找这个颜色，返回**物理像素**框（找不到 → null）。 */
function findColor(color: string): Box | null {
    try {
        const r = cu.match(JSON.stringify({ kind: 'color', color: color }));
        return (r as Box) || null;
    } catch (e) {
        state.errors.push('match 抛错: ' + e);
        return null;
    }
}

function fmt(b: Box): string {
    return '(' + b.x.toFixed(2) + ',' + b.y.toFixed(2) + ' ' + b.w.toFixed(2) + 'x' + b.h.toFixed(2) + ')';
}

/** 隔几拍留一张帧（观测面：人/agent 事后看画面）*/
function shot(tag: string): void {
    try {
        const data: string = cu.screenshot();
        const b64 = data.indexOf(',') >= 0 ? data.slice(data.indexOf(',') + 1) : data;
        fs.writeFileSync(framesDir + '/' + tag + '.png', Buffer.from(b64, 'base64'));
        state.saved++;
    } catch (e) {
        state.errors.push('写帧失败: ' + e);
    }
}

/** 目标窗口在不在（layoutTree 探一下；找不到窗口 → null） */
function windowReady(title: string): boolean {
    try {
        return !!cu.layoutTree(JSON.stringify({ title: title }));
    } catch (e) {
        return false;
    }
}

function finish(reason: string): void {
    const session = {
        script: 'game/hello/play.ts', reason, steps: state.steps, hits: state.hits,
        misses: state.misses, elapsedMs: Date.now() - t0, frames: state.saved,
        errors: state.errors, window: cfg.window, cfg, ts: Date.now(),
    };
    try {
        fs.writeFileSync(OUT + '/session.json', JSON.stringify(session, null, 2) + '\n', 'utf8');
    } catch (e) { /* 写不进去就算了，别把脚本带崩 */ }
    log('[hello-play] 收工: reason=' + reason + ' 拍=' + state.steps
        + ' 点中=' + state.hits + ' 点空=' + state.misses
        + (state.errors.length ? (' 错误=' + state.errors.length) : ''));
}

function main(): void {
    log('[hello-play] 目标窗口 "' + cfg.window + '"，产物目录 ' + OUT);

    // 等窗口出现，再点一下正中把它**激活**（CU 的单击命中窗口会激活它；之后点击才算落在游戏上）
    // ⚠ 坐标是**物理像素** ⇒ 先问一下屏尺寸（scale≠1 时与逻辑尺寸不同）
    const end = Date.now() + 15000;
    while (Date.now() < end && !windowReady(cfg.window))
        sleep(300);
    const si = cu.screenInfo ? cu.screenInfo() : null;
    const sw = (si && si.physical ? si.physical[0] : 800) || 800;
    const sh = (si && si.physical ? si.physical[1] : 600) || 600;
    cu.click(sw / 2, sh / 2, 'left');
    if (cfg.warmupSec > 0)
        sleep(cfg.warmupSec * 1000);

    let reason = 'steps';
    for (let step = 0; ; step++) {
        if (cfg.steps > 0 && step >= cfg.steps) { reason = 'steps'; break; }
        if (cfg.maxSeconds > 0 && (Date.now() - t0) / 1000 >= cfg.maxSeconds) { reason = 'time'; break; }

        const red = findColor(RED);
        const blue = findColor(BLUE);      // 只为日志：蓝球是"不该点"的那个
        if (red) {
            // 点它的**中心**：match 与 click 都是"整块桌面"的**物理像素**坐标，直接对得上
            cu.click(red.x + red.w / 2, red.y + red.h / 2, 'left');
            state.hits++;
        } else {
            state.misses++;
        }
        if (step % 5 === 0)
            shot('f' + String(step).padStart(5, '0'));

        log('step ' + step + ' 红=' + (red ? fmt(red) : '(没找到)')
            + ' 蓝=' + (blue ? fmt(blue) : '(没找到)')
            + (red ? (' → 点(' + (red.x + red.w / 2).toFixed(3) + ',' + (red.y + red.h / 2).toFixed(3) + ')')
                   : ' → 不点(等下一拍)'));

        state.steps++;
        sleep(cfg.stepMs);
    }
    finish(reason);
}

main();
