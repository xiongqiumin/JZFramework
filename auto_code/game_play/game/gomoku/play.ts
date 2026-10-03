// ============================================================================
// game/gomoku/play.ts —— gomoku 玩法的**参考脚本**（会真的下：找盘 → 逐点问子 → 造双杀）
// ============================================================================
// 它演示的是"**只看画面**下一盘五子棋"的路子（也是 `run_veirfy.py` 默认装的脚本）：
//
//   ① 校准：`match({kind:'color',color:'#e3b572'})` 找棋盘底色那块 → 拿到棋盘矩形；
//   ② 读盘：**逐交点问颜色**（每次给一个躲开网格十字的小 region）—— `match` 一次只回一个框，
//      所以"哪一格有子"是一格一格问出来的；只问"已有棋子附近"的那些空点（frontier），省时间；
//   ③ 落子：`click` 交点的归一化坐标（黑先，我们执黑）；落完立刻确认（探一下自己那格），
//      万一坐标有系统偏差就按实际落点把整盘映射纠正过来；
//   ④ 策略：能连五就连五 → 电脑能连五就堵 → 能做出**双杀**（下完有两处都能连五）就做 →
//      否则按"进攻 + 堵截"的价值选点（对手是 1-ply 的启发式机器人，双杀是它的死穴）。
//
// ⚠ 红线（见 prompt.md）：只看得见画面、只操作鼠标键盘；不读容器日志 / 状态文件 / 本体源码。
// ============================================================================
import fs from 'fs';
import { Buffer } from 'buffer';

interface PlayCfg {
    out: string;         // 产物目录（绝对路径）：play.log / frames/ / session.json 都写这儿
    app: string;         // 目标程序名（容器里那个游戏机进程）
    window: string;      // 目标窗口标题
    warmupSec: number;
    steps: number;       // 最多多少拍（0 = 不看拍数）
    stepMs: number;      // 每拍间隔（毫秒）
    maxSeconds: number;  // 最多跑多少秒（0 = 不看时间）—— 本玩法由尺子写成限时(180)
}

const cu = __jz.computeruse;
const dir = jzApp.applicationDirPath();
const cfg = JSON.parse(fs.readFileSync(dir + '/play.json', 'utf8')) as PlayCfg;

// ⚠ 路径自己拼正斜杠（容器里的 path 是 POSIX 口径）
const OUT = String(cfg.out).replace(/\\/g, '/').replace(/\/+$/, '');
const framesDir = OUT + '/frames';
const logPath = OUT + '/play.log';
fs.mkdirSync(framesDir, { recursive: true });

const t0 = Date.now();
const state = { steps: 0, moves: 0, result: '', frames: 0, probes: 0, errors: [] as string[] };

function log(line: string): void {
    const s = '[' + ((Date.now() - t0) / 1000).toFixed(1) + 's] ' + line;
    console.log(s);
    fs.appendFileSync(logPath, s + '\n');
}
const sleep = (ms: number): void => { if (ms > 0) cu.wait(ms); };

// ---------------------------------------------------------------------------
// 题目侧的几何与颜色（与本体 binary/code_agent/runtime/gomoku/main.js 一致，见 prompt.md）
// ---------------------------------------------------------------------------
const GRID = 15, CELL = 40, MARGIN = 20;
const BOARD = MARGIN * 2 + (GRID - 1) * CELL;          // 600
const EMPTY = 0, US = 1 /*黑*/, BOT = 2 /*白*/;
const C_BOARD = '#e3b572';     // 棋盘底（最大的一块 ⇒ 用它校准）
const C_BLACK = '#111111';     // 我们（黑）
const C_WHITE = '#f6f6f6';     // 电脑（白）

interface Box { x: number; y: number; w: number; h: number; }
interface Pt { r: number; c: number; }

/** 在**整块桌面**上找这个颜色（可给 region），返回归一化框（找不到 → null）。 */
function matchColor(color: string, region?: Box): Box | null {
    try {
        const loc: { kind: string; color: string; region?: Box } = { kind: 'color', color: color };
        if (region)
            loc.region = region;
        const r = cu.match(JSON.stringify(loc));
        state.probes++;
        return (r as Box) || null;
    } catch (e) {
        state.errors.push('match 抛错: ' + e);
        return null;
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

// ---------------------------------------------------------------------------
// ① 校准：棋盘矩形（归一化）
// ---------------------------------------------------------------------------
let box: Box | null = null;

function calibrate(deadline: number): boolean {
    while (Date.now() < deadline) {
        const b = matchColor(C_BOARD);
        if (b && b.w > 0.3 && b.h > 0.3) {     // 棋盘占桌面大头，别被零碎同色骗了
            box = b;
            return true;
        }
        sleep(300);
    }
    return false;
}

/** 交点 (r,c) 的桌面归一化坐标；dx/dy = 相对交点的像素偏移（用来躲网格十字）。 */
function pt(r: number, c: number, dx: number, dy: number): { x: number; y: number } {
    const b = box as Box;
    const px = MARGIN + c * CELL + dx, py = MARGIN + r * CELL + dy;
    return { x: b.x + px / BOARD * b.w, y: b.y + py / BOARD * b.h };
}

/**
 * 探一个交点：返回 EMPTY / US / BOT。
 * ⚠ 交点正上是**网格十字**（#3a2a12）——所以取样点往右上偏 (8,-8)px：那里只有棋子或棋盘底。
 */
function probe(r: number, c: number, whiteFirst: boolean): number {
    const b = box as Box;
    const p = pt(r, c, 8, -8);
    const w = 12 * (b.w / BOARD), h = 12 * (b.h / BOARD);   // ~12x12px 的窗口
    const region: Box = { x: p.x - w / 2, y: p.y - h / 2, w: w, h: h };
    if (whiteFirst) {
        if (matchColor(C_WHITE, region)) return BOT;
        if (matchColor(C_BLACK, region)) return US;
    } else {
        if (matchColor(C_BLACK, region)) return US;
        if (matchColor(C_WHITE, region)) return BOT;
    }
    return EMPTY;
}

/** 点一下某个交点（按纠正过的映射点）。 */
function clickCell(r: number, c: number): void {
    const p = pt(r - adjR, c - adjC, 0, 0);
    cu.click(p.x, p.y, 'left');
}

// ---------------------------------------------------------------------------
// ② 棋盘模型（只靠探出来的事实更新；探不到就不猜）
// ---------------------------------------------------------------------------
const model: number[][] = [];
for (let i = 0; i < GRID; i++)
    model.push(new Array(GRID).fill(EMPTY));

let adjR = 0, adjC = 0;      // 坐标映射的系统偏差（按实际落点纠正，单位=格）

function emptyNearby(): Pt[] {          // 空点、且附近 2 格内有子（机器人只会下这种点）
    const out: Pt[] = [];
    for (let r = 0; r < GRID; r++)
        for (let c = 0; c < GRID; c++) {
            if (model[r][c] !== EMPTY) continue;
            let near = false;
            for (let dr = -2; dr <= 2 && !near; dr++)
                for (let dc = -2; dc <= 2; dc++) {
                    const rr = r + dr, cc = c + dc;
                    if (rr >= 0 && cc >= 0 && rr < GRID && cc < GRID && model[rr][cc] !== EMPTY) { near = true; break; }
                }
            if (near) out.push({ r: r, c: c });
        }
    return out;
}

/** 找电脑（白）刚才那手：先问 frontier，找不到就全盘扫一遍（只问白）。 */
function findBotStone(): Pt | null {
    const cands = emptyNearby();
    for (const q of cands)
        if (probe(q.r, q.c, true) === BOT) return q;
    for (let r = 0; r < GRID; r++)
        for (let c = 0; c < GRID; c++)
            if (model[r][c] === EMPTY && probe(r, c, true) === BOT) return { r: r, c: c };
    return null;
}

/** 我们刚点的那手有没有落上？落上了返回 true；落到旁边就把映射纠正过来。 */
function confirmOurMove(r: number, c: number): boolean {
    sleep(160);            // 点击是"送出去就返回"⇒ 等画面更新一下再探
    if (probe(r, c, false) === US) return true;
    for (let dr = -1; dr <= 1; dr++)
        for (let dc = -1; dc <= 1; dc++) {
            if (!dr && !dc) continue;
            const rr = r + dr, cc = c + dc;
            if (rr < 0 || cc < 0 || rr >= GRID || cc >= GRID) continue;
            if (model[rr][cc] === EMPTY && probe(rr, cc, false) === US) {
                log('!! 映射有偏差：想下 (' + r + ',' + c + ')，实际落在 (' + rr + ',' + cc + ') ⇒ 纠正 adj=(' + (adjR + dr) + ',' + (adjC + dc) + ')');
                adjR += dr; adjC += dc;
                model[rr][cc] = US;
                return true;
            }
        }
    return false;
}

// ---------------------------------------------------------------------------
// ④ 策略：连五 > 堵连五 > 双杀 > 价值
// ---------------------------------------------------------------------------
const DIRS: number[][] = [[0, 1], [1, 0], [1, 1], [1, -1]];

/** 假设 m 里 (r,c) 已经是 who 的子，返回四个方向的 [连子数, 活端数]。 */
function statsIfPlayed(m: number[][], r: number, c: number, who: number): number[][] {
    const out: number[][] = [];
    for (const d of DIRS) {
        const dr = d[0], dc = d[1];
        let count = 1, open = 0;
        for (let s = -1; s <= 1; s += 2) {
            let rr = r + dr * s, cc = c + dc * s;
            while (rr >= 0 && cc >= 0 && rr < GRID && cc < GRID && m[rr][cc] === who) {
                count++; rr += dr * s; cc += dc * s;
            }
            if (rr >= 0 && cc >= 0 && rr < GRID && cc < GRID && m[rr][cc] === EMPTY) open++;
        }
        out.push([count, open]);
    }
    return out;
}

/** 这个点落 who 会不会直接连五（不建数组 —— 双杀搜索要调用几万次）。 */
function winsIfPlayed(m: number[][], r: number, c: number, who: number): boolean {
    for (const d of DIRS) {
        const dr = d[0], dc = d[1];
        let count = 1;
        for (let s = -1; s <= 1; s += 2) {
            let rr = r + dr * s, cc = c + dc * s;
            while (rr >= 0 && cc >= 0 && rr < GRID && cc < GRID && m[rr][cc] === who) {
                count++; rr += dr * s; cc += dc * s;
            }
        }
        if (count >= 5) return true;
    }
    return false;
}

/** 落 who 到 (r,c) 的**威胁价值**（多个方向相加 ⇒ 双三/四三 自然比单威胁高）。 */
function threatValue(m: number[][], r: number, c: number, who: number): number {
    let sum = 0;
    for (const s of statsIfPlayed(m, r, c, who)) {
        const n = s[0], open = s[1];
        if (n >= 5) sum += 100000;
        else if (n === 4 && open >= 2) sum += 10000;
        else if (n === 4 && open === 1) sum += 1200;
        else if (n === 3 && open >= 2) sum += 800;
        else if (n === 3 && open === 1) sum += 80;
        else if (n === 2 && open >= 2) sum += 50;
        else if (n === 2 && open === 1) sum += 8;
        else sum += n;
    }
    return sum;
}

function clone(m: number[][]): number[][] {
    const out: number[][] = [];
    for (let r = 0; r < GRID; r++) out.push(m[r].slice());
    return out;
}

/** 落 who 到 (r,c) 之后，who **下一手能连五**的点有几个（≥2 就是双杀）。 */
function winCellsAfter(m: number[][], r: number, c: number, who: number): number {
    const m2 = clone(m);
    m2[r][c] = who;
    let n = 0;
    for (let rr = 0; rr < GRID; rr++)
        for (let cc = 0; cc < GRID; cc++)
            if (m2[rr][cc] === EMPTY && winsIfPlayed(m2, rr, cc, who)) n++;
    return n;
}

function candidates(): Pt[] {
    const cands = emptyNearby();
    if (!cands.length) {
        const mid = (GRID - 1) / 2;
        if (model[mid][mid] === EMPTY) cands.push({ r: mid, c: mid });     // 开局: 天元
    }
    return cands;
}

function chooseMove(): Pt | null {
    const cands = candidates();
    if (!cands.length) return null;

    // 1) 我们能连五 → 直接赢
    for (const q of cands)
        if (winsIfPlayed(model, q.r, q.c, US)) return q;

    // 2) 电脑能连五 → 必须堵（有多个也堵不完，挑它最狠的那个）
    let block: Pt | null = null, blockV = -1;
    for (const q of cands)
        if (winsIfPlayed(model, q.r, q.c, BOT)) {
            const v = threatValue(model, q.r, q.c, BOT);
            if (v > blockV) { blockV = v; block = q; }
        }
    if (block) return block;

    // 3) 能做双杀就做（下完有两处都能连五 ⇒ 机器人只堵得住一处）
    //    双杀判定贵（要看下完之后的全部杀点）⇒ 只查"威胁值最高的前 12 个点"（双杀的威胁值必然高）
    const scored: { q: Pt; v: number }[] = [];
    for (const q of cands)
        scored.push({ q: q, v: threatValue(model, q.r, q.c, US) });
    scored.sort((a, b) => b.v - a.v);
    for (let i = 0; i < scored.length && i < 12; i++) {
        const q = scored[i].q;
        if (winCellsAfter(model, q.r, q.c, US) >= 2) {
            log('找到双杀点 (' + q.r + ',' + q.c + ')（该点威胁值 ' + scored[i].v + '）');
            return q;
        }
    }

    // 4) 挡电脑的"活四/四"（它下一手要形成 4 就得先按住），否则按"进攻 + 堵截"选点
    let best: Pt | null = null, bestV = -1e18;
    for (const q of cands) {
        const attack = threatValue(model, q.r, q.c, US);
        const defend = threatValue(model, q.r, q.c, BOT);
        const center = -(Math.abs(q.r - 7) + Math.abs(q.c - 7)) * 0.5;
        let v = attack * 1.6 + defend * 1.2 + center;
        if (defend >= 1200) v += 2000;          // 电脑在这儿能凑"四" ⇒ 优先堵
        if (v > bestV) { bestV = v; best = q; }
    }
    return best;
}

function hasFive(who: number): boolean {
    for (let r = 0; r < GRID; r++)
        for (let c = 0; c < GRID; c++)
            if (model[r][c] === who) {
                for (const d of DIRS) {
                    const dr = d[0], dc = d[1];
                    let n = 1;
                    for (let s = 1; s < 5; s++) {
                        const rr = r + dr * s, cc = c + dc * s;
                        if (rr < 0 || cc < 0 || rr >= GRID || cc >= GRID || model[rr][cc] !== who) break;
                        n++;
                    }
                    if (n >= 5) return true;
                }
            }
    return false;
}

// ---------------------------------------------------------------------------
// 落盘 / 收工
// ---------------------------------------------------------------------------
function dump(): string {
    const rows: string[] = [];
    for (let r = 0; r < GRID; r++) {
        let s = '';
        for (let c = 0; c < GRID; c++)
            s += (model[r][c] === EMPTY ? '.' : (model[r][c] === US ? 'X' : 'O'));
        rows.push(s);
    }
    return rows.join('/');
}

function shot(tag: string): void {
    try {
        const data: string = cu.screenshot();
        const b64 = data.indexOf(',') >= 0 ? data.slice(data.indexOf(',') + 1) : data;
        fs.writeFileSync(framesDir + '/' + tag + '.png', Buffer.from(b64, 'base64'));
        state.frames++;
    } catch (e) {
        state.errors.push('写帧失败: ' + e);
    }
}

function finish(reason: string): void {
    const session = {
        script: 'game/gomoku/play.ts', reason: reason, result: state.result,
        moves: state.moves, steps: state.steps, probes: state.probes,
        elapsedMs: Date.now() - t0, frames: state.frames, errors: state.errors,
        window: cfg.window, board: dump(), cfg: cfg, ts: Date.now(),
    };
    try {
        fs.writeFileSync(OUT + '/session.json', JSON.stringify(session, null, 2) + '\n', 'utf8');
    } catch (e) { /* 写不进去就算了，别把脚本带崩 */ }
    log('[gomoku-play] 收工: reason=' + reason + ' result=' + state.result
        + ' 手数=' + state.moves + ' 探测=' + state.probes
        + (state.errors.length ? (' 错误=' + state.errors.length) : ''));
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------
function main(): void {
    log('[gomoku-play] 目标窗口 "' + cfg.window + '"，产物目录 ' + OUT);

    // 等窗口出现；点状态栏（y 很小，落在棋盘之外 ⇒ 只激活窗口，不会落子）
    const waitEnd = Date.now() + 15000;
    while (Date.now() < waitEnd && !windowReady(cfg.window))
        sleep(300);
    cu.click(0.5, 0.02, 'left');
    if (cfg.warmupSec > 0)
        sleep(cfg.warmupSec * 1000);

    // ① 校准棋盘
    if (!calibrate(Date.now() + 8000)) {
        state.result = 'no_board';
        finish('no_board');
        return;
    }
    log('棋盘矩形(归一化): ' + (box as Box).x.toFixed(3) + ',' + (box as Box).y.toFixed(3)
        + ' ' + (box as Box).w.toFixed(3) + 'x' + (box as Box).h.toFixed(3));

    const tEnd = cfg.maxSeconds > 0 ? t0 + cfg.maxSeconds * 1000 : 0;
    let reason = 'time';

    for (let step = 0; ; step++) {
        state.steps = step;
        if (tEnd && Date.now() > tEnd - 1500) { reason = 'time'; break; }

        // ② 看电脑刚下的那手（我们上一手之后它立刻回了一手）
        if (state.moves > 0) {
            const b = findBotStone();
            if (b) {
                model[b.r][b.c] = BOT;
                log('电脑(白)落在 (' + b.r + ',' + b.c + ')');
                if (hasFive(BOT)) { state.result = 'ai_win'; reason = 'ai_win'; break; }
            } else if (state.moves > 0) {
                log('!! 没找到电脑的子（frontier 全问过了）——先按现在的盘面继续');
            }
        }

        // ③ 我方决策
        const mv = chooseMove();
        if (!mv) { reason = 'no_move'; break; }
        const before = emptyNearby().length;
        clickCell(mv.r, mv.c);
        if (!confirmOurMove(mv.r, mv.c)) {
            log('!! 落子没确认到 (' + mv.r + ',' + mv.c + ')（重试一次）');
            clickCell(mv.r, mv.c);
            if (!confirmOurMove(mv.r, mv.c)) {
                state.errors.push('落子失败 (' + mv.r + ',' + mv.c + ')');
                continue;
            }
        }
        model[mv.r][mv.c] = US;
        state.moves++;
        const v = threatValue(model, mv.r, mv.c, US);
        log('我(黑)落在 (' + mv.r + ',' + mv.c + ') 手数=' + state.moves
            + ' 该点威胁值=' + v + '（候选空点 ' + before + '）');

        if (hasFive(US)) { state.result = 'human_win'; reason = 'win'; break; }

        if (state.moves % 6 === 0)
            shot('m' + String(state.moves).padStart(3, '0'));
    }

    if (state.result === '') state.result = (reason === 'time' ? 'timeout' : 'unknown');
    shot('final_m' + String(state.moves).padStart(3, '0'));
    finish(reason);
}

try {
    main();
} catch (e) {
    state.errors.push('脚本异常: ' + e);
    log('!! 脚本异常: ' + e);
    try { finish('crash'); } catch (e2) { /* 收工都写不进去就算了 */ }
}
