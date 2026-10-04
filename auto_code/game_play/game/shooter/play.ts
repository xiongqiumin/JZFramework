// ============================================================================
// 飞行射击 玩法 · **参考脚本**（视觉 + computer use，2D 决策）
//
// 红线：**只看得见画面、只动鼠标键盘**；不读容器日志里本体打印的 `[shooter] score=..`
//   （那是尺子的特权），也不碰 game/**。
//
// 它是这份玩法的参考实现（就是这个文件；`run_veirfy.py <run.json> <这个文件>` 会把它拷进复验区
//   编译成 build/verify/play/play.js）。目标：一局之内打到 300 分（30 架敌机）。
//
// **纵向(y)必须参与决策**：本体支持上下（`VK_UP/VK_DOWN/W/S`，见 updateGame 的玩家移动），
//   只按左右的话 800x540 的活动区在决策上退化成一条 800px 的横线 —— "整条线上总有一发弹"，
//   被逼到墙边时**一个方向都不能躲**（早期只按左右的版本实测三次掉命里两次是这种）。所以：
//     ① 自己位置 (x,y) 都从画面取（不假设"恒在底部 60px"）；
//     ② 观测/威胁模型是 **2D**（敌弹有 vx/vy，敌机直着落）；
//     ③ 落点搜索**从贴底那一行开始**，哪一行存在"未来 1.3s 挨不到"的位置就站那儿；
//        整条底线都被弹幕盖住才往上抬（那正是纵向空间该出场的时候）；
//     ④ 判定用**闭式解**（我的路径按拐点切 ≤3 片，每片解"两轴同时进盒"的区间交）——
//        不能只抽几个时刻比对：斜着飞的弹"横向在带里"与"纵向在带里"可能发生在不同时间。
//
// ⚠ 观测/动作都是有延迟的（一拍取一帧、按住键由本体下一帧才动）⇒ 规划时给判定盒留**误差余量**
//   （见 MG），不然会出现"规划上还有 1~2px、实际已经压线"的擦边挨打。
//
// ⚠ **真桌面与容器有硬差别**（这一版两边都能跑，差别的处理见 main 开头那几段）：真桌面上
//   ① 到处都有和游戏同色的像素（任务栏/别的程序）⇒ 游戏区取**窗口客户区**、扫描带 `region`；
//   ② 目标在别的进程、多半没有可访问布局 ⇒ "窗口在不在"查**窗口表**，别只问 layoutTree；
//   ③ 窗口不一定在屏幕中间 ⇒ 激活要点**客户区中心**，不是点屏幕正中心。
//
// ⚠ 红线补充：不 import 题目侧的类型；颜色/尺寸/速度这些**题目给定**的常量照抄 prompt.md
//   （敌机 46x40 / 30x26、命中盒、开火冷却……），不是偷看运行期状态。
//
// 产物：<cfg.out>/play.log、session.json、frames/（收工那一帧叫 end.png，分数/命数都在 HUD 上）。
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
    maxSeconds: number;  // 最多跑多少秒（0 = 不看时间）
}

const cu = __jz.computeruse;
const dir = jzApp.applicationDirPath();
const cfg = JSON.parse(fs.readFileSync(dir + '/play.json', 'utf8')) as PlayCfg;

// ⚠ 路径自己拼正斜杠（容器里的 path 是 POSIX 口径，见 prompt.md）
const OUT = String(cfg.out).replace(/\\/g, '/').replace(/\/+$/, '');
const framesDir = OUT + '/frames';
const logPath = OUT + '/play.log';
fs.mkdirSync(framesDir, { recursive: true });

const t0 = Date.now();
const state = {
    steps: 0, slices: 0, looks: 0, moves: 0, shots: 0, keys: 0,
    sawSelf: 0, sawEnemy: 0, sawBullet: 0,
    sawPw: 0, revives: 0, saved: 0, slowSteps: 0, matchMs: 0, actMs: 0, lookMs: 0,
    upRows: 0,          // 有多少小节用到了"往上抬"(= 纵向空间的真实用量)
    clearedSeen: false,  // 看到过通关遮罩(见 C_CLEAR): 收工用
    errors: [] as string[],
};

function log(line: string): void {
    const s = '[' + ((Date.now() - t0) / 1000).toFixed(1) + 's] ' + line;
    console.log(s);
    // ⚠ 落盘要包起来: 日志失败**绝不能把脚本带崩**（实测逐小节转储跑到一百多行时
    //   appendFileSync 抛过一次异常, 整局直接 script_gone）。逐小节那种又长又密的行只进
    //   console（容器日志能捞到），落盘只留逐拍摘要。
    if (line.slice(0, 5) === '    sl')
        return;
    try {
        fs.appendFileSync(logPath, s + '\n');
    } catch (e) {
        state.errors.push('写日志失败: ' + e);
    }
}
const sleep = (ms: number): void => { if (ms > 0) cu.wait(ms); };

// ---------------------------------------------------------------------------
// 题目给定的几何（**游戏逻辑像素**；prompt.md 有）
// ---------------------------------------------------------------------------
const GW = 800, GH = 600;                       // 客户区逻辑尺寸
const P_MIN_X = 16, P_MAX_X = GW - 16;          // 玩家的横向限位
const P_MIN_Y = 40, P_MAX_Y = GH - 20;          // 玩家的纵向限位（⚠ 上下都能走）
const V_PLAYER = 6 / 16;                        // 6px/逻辑帧(16ms) = 0.375 px/ms
const V_GUN = 11 / 16;                          // 我打出去的子弹: 11px/帧
const V_EBULLET = 4 / 16;                       // 敌方子弹: 4px/帧（只在"还没估出速度"时兜底）
const FIRE_CD = 8 * 16;                         // 开火冷却 8 帧 = 128ms
const E_BIG = { w: 46, h: 40, hp: 3, spd: 0 };  // 大敌机: 3 枪, +10 分
const E_SML = { w: 30, h: 26, hp: 1, spd: 1 };  // 小敌机: 1 枪, +10 分
// 判定盒（本体 hitPlayer / 子弹结算里的那几条）：
const HB_BULLET = { rx: 16, ry: 18 };           // 敌弹 vs 我: |dx|<16 且 |dy|<18
const foeHB = (big: boolean): { rx: number; ry: number } => big
    ? { rx: 23 + 14, ry: 20 + 14 }              // 敌机 vs 我: w/2+14, h/2+14
    : { rx: 15 + 14, ry: 13 + 14 };
const foeAim = (big: boolean): number => (big ? 23 : 15) + 2;   // 我 vs 敌机: |dx| < w/2+3（留 1px 余量）
const PICK = { rx: 18, ry: 20 };                // 拾取道具: |dx|<18 且 |dy|<20
/** 道具值多少（决定要不要为它多走几步）：环射是整圈散射、输出杠杆最大；回血多一次机会。 */
function powWorth(color: string): number {
    if (same(color, C_PW_RING)) return 140;
    if (same(color, C_PW_LIFE)) return 150;
    if (same(color, C_PW_MULTI)) return 110;
    return 0;
}

// 规划用的**位置误差余量**（游戏逻辑像素）：一拍 45ms 里我最多走出 17px、而画面是"拍初"那张
//   ⇒ 规划的位置与真实位置能差十几像素。不留余量就会出现"规划上还有 1~2px、实际已经压线"的
//   擦边挨打（本体那边实测三次掉命全是这种）。
//   给 10px：小于"一拍的最大位移"，又不会把整条 lane 判死。
const MG = 10;
const VERBOSE = false;          // 逐小节转储威胁明细（诊断用；跑分时关掉，省得刷屏耗 CPU）
const HORIZON_MS = 1300;        // 只看未来 1.3s；更远的威胁下一拍重算（那时位置也更准）
const ROW_STEP = 24;            // 往上抬一行的步长（游戏 px）
const MAX_ROWS = 6;             // 最多抬 6 行(144px)
const X_STEP = 8;               // 底边那一行的横向细扫步长
// 往上抬行时只试这几个"逃跑点"（相对当前位置的横向偏移）：密集时刻整行细扫太贵。
const ESCAPE_DX = [0, 32, -32, 64, -64, 96, -96, 128, -128, 160, -160, 192, -192];

// ---------------------------------------------------------------------------
// 观测：一帧 → 游戏坐标下的角色
// ---------------------------------------------------------------------------
interface Box { x: number; y: number; w: number; h: number; }
interface Blk extends Box { area: number; color: string; }

const C_BG = '#0a0c1e';
const C_ENEMY_BIG = '#e65a5a';
const C_ENEMY_SML = '#dc46a0';
const C_SHIP = '#78dcff';
const C_EBULLET = '#ff783c';
const C_PW_MULTI = '#50a0ff';
const C_PW_RING = '#78e678';
const C_PW_LIFE = '#ffdc5a';
// 通关遮罩里"恭喜过关"那行字的描边色（**只在 TS 版有**：到 300 分后世界定格 + 盖一层遮罩）。
// 整个画面里没有第二处用这个亮绿 ⇒ 见到它就等于"这局通关了"。
const C_CLEAR = '#78ff78';
// "确实有字"的判定下限（算的是那一块的 宽x高；见 clearOverlayOn 的说明）。
const CLEAR_AREA = 30;

const same = (a: string, b: string): boolean => String(a).toLowerCase() === b.toLowerCase();
const boxCx = (b: Box): number => b.x + b.w / 2;
const boxCy = (b: Box): number => b.y + b.h / 2;
const boxStr = (b: Box | null): string => b
    ? '(' + b.x.toFixed(0) + ',' + b.y.toFixed(0) + ' ' + b.w.toFixed(0) + 'x' + b.h.toFixed(0) + ')'
    : '(没看到)';

/** 一次取帧、一遍扫描同时判全部关注色（贵的是取帧，所以别一个颜色一次往返）。
 *  ⚠ region = **目标窗口的客户区**（物理像素）。真桌面上这一步是必须的：任务栏图标、
 *     别的程序的深色主题里就有我们要找的颜色（实测 #78dcff 出现在 IDE 的目录树里、
 *     #50a0ff/#78e678 出现在任务栏图标上）—— 不限定搜索区,"我/敌机/道具"全会认错。 */
function lookAll(region: Box | null): Blk[] {
    try {
        const r = cu.matchAll(JSON.stringify({
            colors: [C_SHIP, C_ENEMY_BIG, C_ENEMY_SML, C_PW_RING, C_PW_MULTI, C_PW_LIFE, C_EBULLET],
            // ⚠ 下限放到 2x2: 飞船是**线画**(笔宽 2), 旋转到某些角度时一条线只占 2px 宽,
            //   3x3 的下限会把它整条丢掉 ⇒ 并集少一块, "我在哪"就会跳几十像素(实测 ±25px,
            //   已经超过飞船一拍能走的最大位移 ⇒ 那是估计噪声, 不是真位移)。
            //   真正的噪声靠后面的面积判据挡(见 pickSelf)。
            region: region ? { x: region.x, y: region.y, w: region.w, h: region.h } : undefined,
            all: true, minW: 2, minH: 2, limit: 120,
        }));
        state.looks++;
        return Array.isArray(r) ? (r as Blk[]) : [];
    } catch (e) {
        state.errors.push('matchAll 抛错: ' + e);
        return [];
    }
}

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

// ---------------------------------------------------------------------------
// 键盘：**按住 / 抬起**（投递即返回，不占脚本的循环；见 JZComputerUse.h）
//   ⚠ 两轴各一个状态：游戏是"按住哪个方向键就每帧往那边挪 6px"，同时按住两轴就是斜着走。
// ---------------------------------------------------------------------------
let heldX: '' | 'Left' | 'Right' = '';
let heldY: '' | 'Up' | 'Down' = '';
let firing = false;

function holdX(d: '' | 'Left' | 'Right'): void {
    if (heldX === d)
        return;
    if (heldX) { cu.keyUp(heldX); state.keys++; }
    if (d) { cu.keyDown(d); state.keys++; state.moves++; }
    heldX = d;
}
function holdY(d: '' | 'Up' | 'Down'): void {
    if (heldY === d)
        return;
    if (heldY) { cu.keyUp(heldY); state.keys++; }
    if (d) { cu.keyDown(d); state.keys++; state.moves++; }
    heldY = d;
}
/** 按住空格 = 连续开火（游戏按 8 帧冷却自己连发）。 */
function fireOn(): void {
    if (firing)
        return;
    cu.keyDown('Space');
    firing = true;
    state.keys++;
    state.shots++;
}
/** 复位所有按下的键（收尾 / 重开前必须先做，否则游戏会一直往一边跑）。 */
function releaseAll(): void {
    holdX(''); holdY('');
    if (firing) { cu.keyUp('Space'); firing = false; state.keys++; }
}

function windowReady(title: string): boolean {
    // ⚠ 先查**窗口表**(cu.windows)：真桌面下目标通常是**别的进程**的窗口, 而它多半没有可访问的
    //   布局(UIA 拿不到、Win32 子窗口枚举也是空的) ⇒ 只问 layoutTree 的话"窗口明明在"也判成没到,
    //   于是白等满 15 秒(容器里自家窗口语义树齐全, 这个问题看不出来)。
    try {
        const ws = cu.windows(JSON.stringify({ title: title }));
        if (Array.isArray(ws) && ws.length > 0)
            return true;
    } catch (e) { /* 窗口表这条腿不行就退到布局树 */ }
    try {
        return !!cu.layoutTree(JSON.stringify({ title: title }));
    } catch (e) {
        return false;
    }
}

/** 窗口表里目标窗口那一条（client/frame 都是**物理像素**，与截图/点击同一口径）。 */
interface WinRow { client?: number[]; frame?: number[]; active?: boolean; visible?: boolean; minimized?: boolean; }
function findWindow(title: string): WinRow | null {
    try {
        const ws = cu.windows(JSON.stringify({ title: title })) as WinRow[];
        if (Array.isArray(ws) && ws.length > 0)
            return ws[ws.length - 1];
    } catch (e) { /* 下面返回 null */ }
    return null;
}
/** [x,y,w,h] → Box（顺带挡掉退化值）。 */
function boxOf(a: number[] | undefined): Box | null {
    if (!a || a.length < 4 || !(a[2] > 10) || !(a[3] > 10))
        return null;
    return { x: a[0], y: a[1], w: a[2], h: a[3] };
}

/** 通关遮罩在不在（TS 版到 300 分后: 世界定格 + 盖一层"恭喜过关"）。
 *  ⚠ 只问**这一个颜色**、并且只在"看不到自己"的那一刻问（见调用处）—— 不要塞进 lookAll 的多色
 *     扫描: 那一拍画面里实体块本来就多(定格时更是满屏子弹), 多色结果按面积排序+限条数, 这种
 *     细笔画会被挤掉(实测: 塞进去之后一次都没认出来, 而通关帧里那行字明明有 2900 多像素纯色)。
 *  判据取"最大那块够不够大": 笔画会碎成若干小块, 30 只是挡噪声的下限。 */
function clearOverlayOn(region: Box | null): boolean {
    try {
        const r = cu.match(JSON.stringify({
            kind: 'color', color: C_CLEAR,
            region: region ? { x: region.x, y: region.y, w: region.w, h: region.h } : undefined,
        })) as Box | null;
        return !!r && r.w * r.h >= CLEAR_AREA;
    } catch (e) {
        return false;
    }
}

function finish(reason: string): void {
    const session = {
        script: 'game/shooter/play.ts', reason,
        steps: state.steps, slices: state.slices, looks: state.looks, moves: state.moves,
        shots: state.shots, keys: state.keys, sawSelf: state.sawSelf, sawEnemy: state.sawEnemy,
        sawBullet: state.sawBullet, sawPw: state.sawPw, revives: state.revives,
        matchMs: state.matchMs, actMs: state.actMs, lookMs: state.lookMs,
        upRows: state.upRows, elapsedMs: Date.now() - t0, frames: state.saved,
        slowSteps: state.slowSteps, errors: state.errors.slice(-20),
        window: cfg.window, cfg, ts: Date.now(),
    };
    try {
        fs.writeFileSync(OUT + '/session.json', JSON.stringify(session, null, 2) + '\n', 'utf8');
    } catch (e) { /* 写不进去就算了，别把脚本带崩 */ }
    shot('end');                     // 收工那一帧(诊断: 分数/命数都在 HUD 上, 一眼能对)
    releaseAll();
    log('[shooter-vision] 收工: reason=' + reason + ' 拍=' + state.steps + ' 小节=' + state.slices
        + ' 看=' + state.looks + ' 换向=' + state.moves + ' 按键=' + state.keys
        + ' 开火=' + state.shots + ' 见自己=' + state.sawSelf + ' 见敌=' + state.sawEnemy
        + ' 见弹=' + state.sawBullet + ' 见道具=' + state.sawPw + ' 重生=' + state.revives
        + ' 抬行=' + state.upRows + ' 超时拍=' + state.slowSteps
        + (state.errors.length ? (' 噪声/错误=' + state.errors.length) : ''));
}

// ---------------------------------------------------------------------------
// 2D 判定：时间片 [ta,tb] 内我这个点会不会撞进匀速盒
// ---------------------------------------------------------------------------
/** 我在 t(ms) 时某一轴的位置：从 p0 匀速走到 pEnd，用 kn 毫秒走完（之后停住）。 */
function pAt(p0: number, pEnd: number, kn: number, t: number): number {
    if (kn <= 0 || t >= kn)
        return pEnd;
    return p0 + (pEnd - p0) * (t / kn);
}

interface Th {
    rx: number; ry: number;         // 判定盒半宽/半高（含我的判定盒与误差余量）
    x: number; y: number;           // 中心（现在）
    vx: number; vy: number;         // px/ms
    t0: number; t1: number;         // 可能挨到我的时间窗（ms，已与 [0,HORIZON_MS] 取交）
    qxlo: number; qxhi: number; qylo: number; qyhi: number;   // 窗口内它扫过的包围盒（粗筛用）
    foe: boolean; hp: number; big: boolean;
}

/** 时间片内会不会进盒；会 → 最早挨上的时刻（ms，相对现在），不会 → -1。
 *  片内我与威胁都是匀速直线（片边界就是我的拐点）⇒ 两轴各解一个区间再取交，精确、O(1)。 */
function sliceHit(px0: number, pendX: number, knX: number,
                  py0: number, pendY: number, knY: number,
                  th: Th, ta: number, tb: number): number {
    const vpx = (knX > ta) ? (pendX - px0) / knX : 0;
    const vpy = (knY > ta) ? (pendY - py0) / knY : 0;
    const ax = pAt(px0, pendX, knX, ta) - (th.x + th.vx * ta);
    const ay = pAt(py0, pendY, knY, ta) - (th.y + th.vy * ta);
    const bx = vpx - th.vx, by = vpy - th.vy;
    const EPS = 1e-9;
    let xl: number, xh: number;
    if (bx > -EPS && bx < EPS) {
        if (ax <= -th.rx || ax >= th.rx) return -1;
        xl = -1e9; xh = 1e9;
    } else {
        const s1 = (-th.rx - ax) / bx, s2 = (th.rx - ax) / bx;
        xl = s1 < s2 ? s1 : s2; xh = s1 < s2 ? s2 : s1;
    }
    let yl: number, yh: number;
    if (by > -EPS && by < EPS) {
        if (ay <= -th.ry || ay >= th.ry) return -1;
        yl = -1e9; yh = 1e9;
    } else {
        const s1 = (-th.ry - ay) / by, s2 = (th.ry - ay) / by;
        yl = s1 < s2 ? s1 : s2; yh = s1 < s2 ? s2 : s1;
    }
    let lo = xl > yl ? xl : yl;
    if (lo < 0) lo = 0;
    let hi = xh < yh ? xh : yh;
    if (hi > tb - ta) hi = tb - ta;
    if (lo > hi) return -1;          // 这一片里"横向在带里"与"纵向在带里"没有重合（擦肩而过）
    return ta + lo;
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------
function main(): void {
    log('[shooter-vision] 目标窗口 "' + cfg.window + '"，产物目录 ' + OUT);

    const end = Date.now() + 15000;
    while (Date.now() < end && !windowReady(cfg.window))
        sleep(300);

    const si = cu.screenInfo ? cu.screenInfo() : null;
    const sw = (si && si.physical ? si.physical[0] : 800) || 800;
    const sh = (si && si.physical ? si.physical[1] : 600) || 600;
    // 目标窗口那一条（都在**物理像素**口径上，与截图/点击一致）。后面三处要用它：
    //   ① 激活窗口时点它客户区中心；② 当"游戏区"框（画面就是客户区）；③ 兜底校准时限定搜索区。
    const win0 = findWindow(cfg.window);
    // 激活窗口（这个玩法不吃鼠标，没有副作用）：
    //   ① 先按标题置前（focusWindow 返回"它真是前台吗"）；
    //   ② 再点它**客户区的中心** —— 真桌面下窗口不一定在屏幕中间, 点屏幕正中心可能点到别人的
    //      窗口上(顺手把前台抢走 ⇒ 后面所有按键都打到别处)；查不到窗口才退回"点屏幕中心"。
    try { cu.focusWindow(cfg.window); } catch (e) { /* 置前失败就靠下面那一击 */ }
    const cliBox0 = boxOf(win0 ? win0.client : undefined);
    if (cliBox0)
        cu.click(cliBox0.x + cliBox0.w / 2, cliBox0.y + cliBox0.h / 2, 'left');
    else
        cu.click(sw / 2, sh / 2, 'left');
    // 先按一下 R：局已经结束就重开、局还活着时是空操作 ⇒ 两个方向都安全，还省掉开局白等的那几秒。
    cu.key('R');
    sleep(60);
    cu.key('Return');
    sleep(200);
    fireOn();

    // 游戏区（屏幕物理像素 → 游戏逻辑像素的原点与缩放都从它来）：
    //   ⭐ **优先用目标窗口的客户区**。容器里"按背景色找整块区域"够用（整块桌面上只有这个游戏），
    //      真桌面不行 —— 任务栏/别的程序的深色主题里就有那个背景色，实测 `cu.match` 返回
    //      (0,0,1920x1020) 的整块屏，于是 SC/原点全错、坐标整体错位。
    //   退路（拿不到窗口表）= 原来的"背景色块"校准，但**搜索区限定在目标窗口的外框里**，并且
    //      要求量出来的长宽比也像游戏区（否则又是"别的窗口的深色"）。
    const board: Box = { x: 0, y: 0, w: sw, h: sh };
    const frameBox0 = boxOf(win0 ? win0.frame : undefined);
    let calibrated = false;
    if (cliBox0 && Math.abs(cliBox0.w / cliBox0.h - GW / GH) < 0.15) {
        board.x = cliBox0.x; board.y = cliBox0.y; board.w = cliBox0.w; board.h = cliBox0.h;
        calibrated = true;
    }
    const calibrate = (): boolean => {
        let bg: Box | null = null;
        try {
            const opts: { kind: string; color: string; region?: Box } = { kind: 'color', color: C_BG };
            if (frameBox0)
                opts.region = frameBox0;
            const r = cu.match(JSON.stringify(opts));
            bg = (r as Box) || null;
        } catch (e) { bg = null; }
        if (!bg || bg.w * bg.h < 100000)
            return false;
        if (Math.abs(bg.w / bg.h - GW / GH) > 0.15)
            return false;
        board.x = bg.x; board.y = bg.y; board.w = bg.w; board.h = bg.h;
        return true;
    };
    if (!calibrated)
        calibrated = calibrate();
    // 屏幕物理像素 → 游戏逻辑像素
    let SC = board.w / GW;
    if (!(SC > 0.2 && SC < 5)) SC = 1;
    const gx = (x: number): number => (x - board.x) / SC;
    const gy = (y: number): number => (y - board.y) / SC;
    log('[shooter-vision] 游戏区 = (' + board.x + ',' + board.y + ' '
        + board.w.toFixed(0) + 'x' + board.h.toFixed(0) + ') 缩放 SC=' + SC.toFixed(3)
        + (calibrated ? '' : '（还没画出来，先用整块桌面兜底）'));

    // ⏱ 一小节的节拍：观测/决策的频率上限。按键不再阻塞之后循环会自己跑到上百 Hz，而每次
    //   "取帧 + 扫色"都是整块桌面（与游戏同进程抢 CPU）⇒ 钉一个节拍。60ms(≈17Hz)：
    //   视界是 1.3s，再快也换不来更好的反应，却会把游戏（和桌面合成）压得更容易停顿 ——
    //   实测停顿期间我看到的是一张**冻住的旧帧**（`我=-`、威胁=0），而游戏照常在跑。
    const sliceMs = 60;

    let selfX = GW / 2, selfY = P_MAX_Y;
    let sawSelfOnce = false;
    let lastSelfMs = Date.now();
    let reason = 'steps';
    let act = '-';
    let lastMe: Box | null = null;
    const wander = { dir: 1, tick: 0 };

    // ---- 轨迹（速度只从画面来：两拍之间的位移 ÷ 间隔；不外推游戏内部公式）----
    // ⚠ 速度单位是 **px/毫秒**（与下面 2D 判定同一套；1 逻辑帧 = 16ms）——写成 px/秒的话，
    //   "时间窗"那几处除法会整体差 1000 倍、窗口塌成一个点，弹刚被配上轨迹就被丢掉
    //   （表现为"看得见弹、却永远不躲"）。诊断串里 v*16 = px/逻辑帧，正好和本体 [hit] 日志同口径。
    // 只用颜色做身份：三类颜色互不相同（大/小敌机、敌弹、三种道具），配错的可能性很低。
    type Kind = 'foe' | 'bullet' | 'pow';
    interface Trk { x: number; y: number; vx: number; vy: number; tMs: number; vw: number; vh: number; color: string; kind: Kind; }
    const kindOf = (color: string): Kind => {
        if (same(color, C_ENEMY_BIG) || same(color, C_ENEMY_SML)) return 'foe';
        if (same(color, C_EBULLET)) return 'bullet';
        return 'pow';
    };
    const isWatched = (color: string): boolean =>
        same(color, C_ENEMY_BIG) || same(color, C_ENEMY_SML) || same(color, C_EBULLET) ||
        same(color, C_PW_MULTI) || same(color, C_PW_RING) || same(color, C_PW_LIFE);
    let trks: Trk[] = [];

    const trackAll = (bs: Blk[], nowMs: number): void => {
        const used: boolean[] = [];
        for (let i = 0; i < trks.length; i++) used.push(false);
        const next: Trk[] = [];
        for (const b of bs) {
            if (!isWatched(b.color)) continue;
            if (b.area < 6) continue;
            const cx = gx(boxCx(b)), cy = gy(boxCy(b));
            let best = -1, bestD = 1e9;
            for (let i = 0; i < trks.length; i++) {
                if (used[i]) continue;
                const t = trks[i];
                if (!same(t.color, b.color)) continue;
                const dt = nowMs - t.tMs;                     // ms（速度就是 px/ms）
                const d = Math.abs(cx - (t.x + t.vx * dt)) + Math.abs(cy - (t.y + t.vy * dt));
                if (d < bestD) { bestD = d; best = i; }
            }
            // 门限按"一拍能走多远"放：敌弹 ~11px/拍，用 40px 足够，又不至于把两个实体串成一条。
            if (best >= 0 && bestD < 40) {
                const t = trks[best]; used[best] = true;
                const dt = Math.max(20, nowMs - t.tMs);       // ms；下限 20ms 防除零
                const vx = (cx - t.x) / dt, vy = (cy - t.y) / dt;
                // 速度平滑（旧 40% + 新 60%）：单拍位移有量化噪声，直接外推会抖。
                next.push({ x: cx, y: cy, tMs: nowMs, color: b.color, kind: kindOf(b.color),
                            vw: Math.abs(b.w) / SC, vh: Math.abs(b.h) / SC,
                            vx: t.vx * 0.4 + vx * 0.6, vy: t.vy * 0.4 + vy * 0.6 });
            } else {
                // 新出现的实体：速度先用**题目给定**的名义值兜底（下一拍配对成功就换成实测值）。
                //   · 敌机/道具只会往下掉 ⇒ (0, 名字速度);
                //   · ⚠ 敌弹**不能**当"直着往下掉": 它是**瞄准我**打出来的 ⇒ 方向取"它现在指向我"的
                //     单位向量、速度取题目给定的 4px/帧。不这么做的话, 新出现的那一拍会把一枚
                //     横着扫过来的弹预测成"往下掉", 于是站在原地等它扫过来(实测失败局全是这种:
                //     `[hit] 中弹 … vy=0`, 而我的模型以为它 vy=+4)。
                let vx = 0, vy = 2 / 16;
                if (same(b.color, C_EBULLET)) {
                    const dx = selfX - cx, dy = selfY - cy;
                    const len = Math.sqrt(dx * dx + dy * dy) + 1e-3;
                    vx = dx / len * V_EBULLET;
                    vy = dy / len * V_EBULLET;
                } else if (same(b.color, C_ENEMY_SML)) {
                    vy = 3 / 16;
                }
                next.push({ x: cx, y: cy, tMs: nowMs, color: b.color, kind: kindOf(b.color),
                            vw: Math.abs(b.w) / SC, vh: Math.abs(b.h) / SC, vx: vx, vy: vy });
            }
        }
        trks = next;
        let ne = 0, nb = 0, np = 0;
        for (const t of trks) {
            if (t.kind === 'foe') ne++;
            else if (t.kind === 'bullet') nb++;
            else np++;
        }
        state.sawEnemy += ne; state.sawBullet += nb; state.sawPw += np;
    };

    const ths: Th[] = [];
    const buildThreats = (): void => {
        ths.length = 0;
        const winMin = (q: number, v: number, r: number, lo: number, hi: number): number =>
            Math.min((lo - r - q) / v, (hi + r - q) / v);
        const winMax = (q: number, v: number, r: number, lo: number, hi: number): number =>
            Math.max((lo - r - q) / v, (hi + r - q) / v);
        for (const t of trks) {
            const foe = t.kind === 'foe';
            if (t.kind === 'pow') continue;                  // 道具不是威胁
            const big = foe && same(t.color, C_ENEMY_BIG);
            const hb = foe ? foeHB(big) : HB_BULLET;
            const rx = hb.rx + MG, ry = hb.ry + MG;
            if (foe && t.vy <= 0.001) continue;              // 敌机只会往下掉；没有速度的先不算
            if (!foe && Math.abs(t.vy) < 0.001 && Math.abs(t.vx) < 0.001) continue;   // 静止块当噪声
            // 时间窗：我的活动区是 [P_MIN_X,P_MAX_X]x[P_MIN_Y,P_MAX_Y]，单轴上它得落进
            // [lo-r, hi+r] 才有戏；两轴取交（t=0 = 现在）。
            let a = 0, b = HORIZON_MS;
            if (Math.abs(t.vx) > 0.001) {
                a = Math.max(a, winMin(t.x, t.vx, rx, P_MIN_X, P_MAX_X));
                b = Math.min(b, winMax(t.x, t.vx, rx, P_MIN_X, P_MAX_X));
            } else if (t.x < P_MIN_X - rx || t.x > P_MAX_X + rx) continue;
            if (Math.abs(t.vy) > 0.001) {
                a = Math.max(a, winMin(t.y, t.vy, ry, P_MIN_Y, P_MAX_Y));
                b = Math.min(b, winMax(t.y, t.vy, ry, P_MIN_Y, P_MAX_Y));
            } else if (t.y < P_MIN_Y - ry || t.y > P_MAX_Y + ry) continue;
            if (a > b) continue;
            const xa = t.x + t.vx * a, xb = t.x + t.vx * b;
            const ya = t.y + t.vy * a, yb = t.y + t.vy * b;
            ths.push({ rx: rx, ry: ry, x: t.x, y: t.y, vx: t.vx, vy: t.vy,
                       qxlo: Math.min(xa, xb) - rx, qxhi: Math.max(xa, xb) + rx,
                       qylo: Math.min(ya, yb) - ry, qyhi: Math.max(ya, yb) + ry,
                       t0: a, t1: b, foe: foe, hp: foe ? (big ? E_BIG.hp : E_SML.hp) : 0, big: big });
        }
    };

    // ---- 选落脚点 (x,y)：从贴底那一行开始，找到"未来 1.3s 挨不到"的位置 ----
    // 贴底优先的道理：底边离敌机最远 ⇒ 打掉它的时间最充裕、被撞的概率最低。
    let bestX = selfX, bestY = selfY, bestDanger = 0, bestCost = 0;

    /** 评估一个落脚点（把 danger/cost 写进 bestDanger/bestCost）。 */
    const evalSpot = (x: number, y: number): void => {
        const knX = Math.abs(x - selfX) / V_PLAYER;
        const knY = Math.abs(y - selfY) / V_PLAYER;
        const need = knX > knY ? knX : knY;            // 两轴同时按 ⇒ 到点看较慢那一轴
        let danger = 0, gain = 0;
        for (let i = 0; i < ths.length; i++) {
            const th = ths[i];
            // 粗筛：我这一趟的路径（一条线段）与它在窗口内扫过的包围盒不相交 ⇒ 一定挨不上。
            // ⚠ 这里是**点**对**盒**——判定盒的膨胀（含我的判定盒与误差余量）都在 rx/ry 里了。
            const myXL = selfX < x ? selfX : x, myXH = selfX < x ? x : selfX;
            if (myXL > th.qxhi || myXH < th.qxlo) continue;
            const myYL = selfY < y ? selfY : y, myYH = selfY < y ? y : selfY;
            if (myYL > th.qyhi || myYH < th.qylo) continue;
            if (th.foe) {
                // 敌机：站进它的命中窗就能打掉它（+10 分，威胁消失）—— 但得**来得及**：
                //   到了之后还要有 killMs（血 × 冷却 + 子弹飞上去的时间）。
                const tArr = (y - th.y) / th.vy;
                if (tArr > 0 && Math.abs(x - th.x) <= foeAim(th.big)) {
                    const fly = (y - 18 - th.y) / (V_GUN + th.vy);
                    const killMs = th.hp * FIRE_CD + fly + 60;
                    if (tArr - need > killMs) {
                        gain += 10 + Math.max(0, 1000 - tArr) / 100;   // 越快到我这高度的越该先打
                        continue;                                      // 能打掉 ⇒ 是收益，不是威胁
                    }
                }
            }
            // 精确判定：路径按两个拐点切成 ≤3 片
            const k1 = knX < knY ? knX : knY, k2 = knX < knY ? knY : knX;
            let hitT = -1;
            const p1 = k1 < th.t1 ? k1 : th.t1;
            if (th.t0 < p1)
                hitT = sliceHit(selfX, x, knX, selfY, y, knY, th, th.t0, p1);
            if (hitT < 0 && k1 < k2) {
                const a2 = k1 > th.t0 ? k1 : th.t0, b2 = k2 < th.t1 ? k2 : th.t1;
                if (a2 < b2)
                    hitT = sliceHit(selfX, x, knX, selfY, y, knY, th, a2, b2);
            }
            if (hitT < 0 && k2 < th.t1) {
                const a3 = k2 > th.t0 ? k2 : th.t0;
                if (a3 < th.t1)
                    hitT = sliceHit(selfX, x, knX, selfY, y, knY, th, a3, th.t1);
            }
            if (hitT >= 0) {
                danger += 100 * (1 + 900 / Math.max(120, hitT));
                if (danger >= 100) break;                 // 已经踩进去了 ⇒ 剩下的不必再算
            } else {
                // 没挨上，但要留余量：在窗口两端与两个拐点上量一下"盒外还剩多少"
                let gap = 1e9;
                for (let s = 0; s < 4; s++) {
                    const tau = s === 0 ? th.t0 : (s === 1 ? k1 : (s === 2 ? k2 : th.t1));
                    if (tau < th.t0 || tau > th.t1) continue;
                    const dx = Math.abs(pAt(selfX, x, knX, tau) - (th.x + th.vx * tau)) - th.rx;
                    const dy = Math.abs(pAt(selfY, y, knY, tau) - (th.y + th.vy * tau)) - th.ry;
                    const g = dx > dy ? dx : dy;
                    if (g < gap) gap = g;
                }
                if (gap < 6)
                    danger += 3 * (1 + 900 / Math.max(120, th.t0));
            }
        }
        // 道具：只在这点"本来就干净"时顺路接（回血 +1 命 / 环射整圈 / 多发 5 路，都很值钱）
        if (danger <= 0.01) {
            for (let i = 0; i < trks.length; i++) {
                const t = trks[i];
                if (t.kind !== 'pow') continue;
                if (Math.abs(x - t.x) > PICK.rx - 2) continue;       // 拾取判定 |dx|<18
                const vu = t.vy > 0 ? t.vy : 2 / 16;
                const tU = (y - t.y) / vu;                           // 它落到我这条 y 上的时刻
                if (tU < 0 || tU > 1200 || need > tU) continue;      // 已落过去 / 太久 / 来不及对准
                gain += powWorth(t.color);
            }
        }
        let c = danger + Math.abs(x - selfX) / 20 + Math.abs(y - selfY) / 20 - gain;
        const edge = Math.min(x - P_MIN_X, P_MAX_X - x);
        if (edge < 40) c += (40 - edge) / 12;            // 别贴横向墙（纵向还有退路，横向被截断更难）
        bestDanger = danger;
        bestCost = c;
    };

    let step = 0;
    let lastClean = false, lastDump = '';      // 诊断: 上一小节的"是否找到干净行"和威胁明细
    const tEnd = cfg.maxSeconds > 0 ? Date.now() + cfg.maxSeconds * 1000 : 0;
    while (true) {
        if (state.clearedSeen) { reason = 'cleared'; break; }
        if (cfg.steps > 0 && step >= cfg.steps) { reason = 'steps'; break; }
        if (tEnd && Date.now() >= tEnd) { reason = 'time'; break; }
        const tStep = Date.now();
        let slices = 0;

        while (Date.now() - tStep < cfg.stepMs) {
            const tSlice = Date.now();

            // ---- 看一眼 ----
            const tLook = Date.now();
            const bs = lookAll(board);       // 搜索区 = 目标窗口客户区（见 lookAll 的说明）
            const lookMs = Date.now() - tLook;
            state.slices++; slices++;
            state.lookMs += lookMs;
            if (!calibrated && (state.slices % 20) === 0) {
                if (calibrate()) {
                    SC = board.w / GW;
                    if (!(SC > 0.2 && SC < 5)) SC = 1;
                    calibrated = true;
                    log('  校准到游戏区 (' + board.x + ',' + board.y + ' '
                        + board.w.toFixed(0) + 'x' + board.h.toFixed(0) + ') SC=' + SC.toFixed(3));
                }
            }
            trackAll(bs, Date.now());

            // ---- 拆角色（全部换算到游戏逻辑像素）----
            let me: Box | null = null;
            let shipRaw = '';
            {
                let l = 1e9, r = -1e9, tp = 1e9, bt = -1e9, n = 0;
                for (const b of bs) {
                    if (!same(b.color, C_SHIP) || b.area < 4) continue;
                    // ⚠ HUD 的"生命图标"也是这个颜色、画在客户区顶部(y≈10) ⇒ 按高度排除它。
                    //   玩家纵向限位是 y∈[40,580]，所以画面上 y<32 的同色块都不是飞船。
                    if (gy(boxCy(b)) < 32) continue;
                    const x0 = gx(b.x), y0 = gy(b.y), x1 = gx(b.x + b.w), y1 = gy(b.y + b.h);
                    if (x0 < l) l = x0;
                    if (x1 > r) r = x1;
                    if (y0 < tp) tp = y0;
                    if (y1 > bt) bt = y1;
                    n++;
                    if (VERBOSE)
                        shipRaw += '(' + x0.toFixed(0) + ',' + y0.toFixed(0) + ' '
                            + (x1 - x0).toFixed(0) + 'x' + (y1 - y0).toFixed(0) + ' a' + b.area + ')';
                }
                if (n) me = { x: l, y: tp, w: r - l, h: bt - tp };
            }
            if (me) {
                selfX = me.x + me.w / 2;
                selfY = me.y + me.h / 2;
                sawSelfOnce = true;
                lastSelfMs = Date.now();
                lastMe = me;
                state.sawSelf++;
            }

            // ---- 威胁表 + 落点搜索 ----
            buildThreats();
            let cleanX = selfX, cleanY = selfY, cleanCost = 1e18, haveClean = false;
            let anyX = selfX, anyY = selfY, anyCost = 1e18;
            for (let row = 0; row < MAX_ROWS; row++) {
                const y = P_MAX_Y - row * ROW_STEP;
                if (y < P_MIN_Y) break;
                let rowCleanX = -1, rowCleanC = 1e18, rowAnyX = -1, rowAnyC = 1e18;
                if (row === 0) {
                    for (let x = P_MIN_X; x <= P_MAX_X; x += X_STEP) {
                        evalSpot(x, y);
                        if (bestCost < rowAnyC) { rowAnyC = bestCost; rowAnyX = x; }
                        if (bestDanger <= 0.01 && bestCost < rowCleanC) { rowCleanC = bestCost; rowCleanX = x; }
                    }
                } else {
                    for (let i = 0; i < ESCAPE_DX.length; i++) {
                        const x = selfX + ESCAPE_DX[i];
                        if (x < P_MIN_X || x > P_MAX_X) continue;
                        evalSpot(x, y);
                        if (bestCost < rowAnyC) { rowAnyC = bestCost; rowAnyX = x; }
                        if (bestDanger <= 0.01) { rowCleanX = x; rowCleanC = bestCost; break; }
                    }
                }
                if (rowCleanX >= 0) {
                    cleanX = rowCleanX; cleanY = y; cleanCost = rowCleanC; haveClean = true;
                    if (row > 0) state.upRows++;
                    break;                                   // 这一行干净 ⇒ 就它（更高的行不用看）
                }
                if (rowAnyC < anyCost) { anyCost = rowAnyC; anyX = rowAnyX; anyY = y; }
            }
            if (haveClean) { bestX = cleanX; bestY = cleanY; }
            else { bestX = anyX; bestY = anyY; }              // 整屏都不干净：退而求其次（代价最小的那个）

            // 诊断（每小节一行，压成一条短串）：我看到的威胁（速度换算成 px/逻辑帧，好和游戏
            // 自己的 [hit] 日志对比）。
            lastClean = haveClean;
            {
                let s = '';
                for (let i = 0; i < ths.length && i < 10; i++) {
                    const t = ths[i];
                    s += (t.foe ? (t.big ? 'B' : 'S') : 'b')
                        + '(' + t.x.toFixed(0) + ',' + t.y.toFixed(0) + ','
                        + (t.vx * 16).toFixed(1) + ',' + (t.vy * 16).toFixed(1) + ') ';
                }
                lastDump = s;
            }
            // 逐小节转储（诊断用；看"被挨打前一小节"模型到底知不知道那发弹）：默认开着，
            //   跑通/调完把 VERBOSE 关掉即可（它只是日志，不参与决策）。
            if (VERBOSE) {
                log('    sl' + state.slices + ' 我=' + (me ? selfX.toFixed(0) + ',' + selfY.toFixed(0) : '-')
                    + ' 目标=' + bestX.toFixed(0) + ',' + bestY.toFixed(0)
                    + ' 干净=' + (haveClean ? 'Y' : 'N') + ' 威胁=' + ths.length
                    + ' 飞船碎片=' + (shipRaw || '-')
                    + ' | ' + lastDump);
            }

            // ---- 落键 ----
            const tAct = Date.now();
            if (me) {
                holdX(bestX > selfX + 4 ? 'Right' : (bestX < selfX - 4 ? 'Left' : ''));
                holdY(bestY > selfY + 4 ? 'Down' : (bestY < selfY - 4 ? 'Up' : ''));
                act = '→(' + bestX.toFixed(0) + ',' + bestY.toFixed(0) + ')'
                    + (bestY < P_MAX_Y - ROW_STEP + 1 ? '↑抬行' : '');
            } else {
                // ⚠ 看不见自己时**不要松键**：受击无敌的隐藏帧、以及桌面合成偶发停顿（那时我看的是
                //   一张冻住的旧帧）都会走到这儿 —— 而停住就等于**站桩挨打**（键全松、也不再校正位置）。
                //   继续按上一拍的方向走，至少还在动；下一拍看到自己就把位置校正回来。
                act = '看不见自己(保持上一拍)';
            }
            state.actMs += Date.now() - tAct;
            state.matchMs += lookMs + (Date.now() - tAct - tLook);

            // ---- 阵亡 → 重开（判据是**时间**：受击无敌 1.44s 会整帧隐藏）----
            const deadLimitMs = sawSelfOnce ? 2000 : 1200;
            const goneMs = Date.now() - lastSelfMs;
            if (goneMs >= deadLimitMs) {
                // ⚠ 先分清"真阵亡"与"**通关定格**": 后者(TS 版到 300 分)世界停住 + 盖一层
                //   "恭喜过关", 画面同样读不到自己 —— 老版在这里直接按 R, 等于**把已经通关的
                //   那一局重开了**(实测容器里: 尺子记到了 CLEAR, 脚本自己又把局开回去)。
                //   判据就是那行字的亮绿(整屏只有它用这个色), 且只在"看不到自己"这一刻问一次。
                if (clearOverlayOn(board)) {
                    log('  → 看到"恭喜过关"（通关 300），收工');
                    state.clearedSeen = true;
                    break;                       // 跳出小节循环; 外层按 reason='cleared' 收工
                }
                log('  → 连着 ' + (goneMs / 1000).toFixed(1) + 's 看不到自己（判定阵亡），按 R 重开');
                releaseAll();
                cu.key('R');
                sleep(120);
                cu.key('Return');
                state.revives++;
                lastSelfMs = Date.now();
                selfX = GW / 2; selfY = P_MAX_Y;
                trks = [];
                sleep(350);
                fireOn();
            }

            const rest = sliceMs - (Date.now() - tSlice);
            if (rest > 0)
                sleep(rest);
        }

        if (step % 5 === 0)
            shot('f' + String(step).padStart(5, '0'));

        log('step ' + step + ' 小节=' + slices
            + ' 看均=' + (state.slices ? (state.lookMs / state.slices).toFixed(0) : '-') + 'ms'
            + ' 我=' + (lastMe ? '(' + selfX.toFixed(0) + ',' + selfY.toFixed(0) + ')'
                               : (sawSelfOnce ? '(藏起来了)' : '(还没见过)'))
            + ' 威胁=' + ths.length + ' → ' + act + ' 开火=' + state.shots);

        state.steps++;
        // ⚠ **循环变量要自己走**: 只加 state.steps 的话 `step >= cfg.steps` 永远不成立 ⇒ 脚本永远不
        //   收工, 只会被外部超时杀掉 —— 容器版一直如此(尺子按预算杀它, 所以没人注意), 原生这边很
        //   刺眼: 收工行不出现、按住的键也留在了游戏里(脚本被强杀时不走 releaseAll)。
        step++;
        const used = Date.now() - tStep;
        if (used > cfg.stepMs)
            state.slowSteps++;
        if (cfg.stepMs > used)
            sleep(cfg.stepMs - used);
    }
    finish(reason);
}

main();
