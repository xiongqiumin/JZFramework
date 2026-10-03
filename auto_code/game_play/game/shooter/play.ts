// ============================================================================
// game/shooter/play.ts —— shooter 玩法的**参考脚本**（会真的打：找敌机 → 对准 → 开火）
// ============================================================================
// 它演示的是"**只看画面**打这个飞行射击"的路子（也是 `run_veirfy.py` 默认装的脚本）：
//
//   ① 开局**立刻进场**（游戏从窗口出现那一刻就开始扣命，玩家不动就是挨撞）；并先按一下 R
//      —— 局已经结束的话它重开（局还活着时 R 是空操作，见本体的 onKeyPress）。
//   ② 校准：`match({kind:'color',color:'#0a0c1e'})` 找星空背景那块**最大连通块** = 游戏区
//      （窗口在桌面上的位置/大小全靠它，不假设原点）；校准前先用整块桌面兜底。
//   ③ ⭐ **一小节只取一帧**：`matchAll({colors:[...], all:true, minW, minH})` —— 一次取帧、
//      一遍扫描同时判**全部**颜色。⚠ 这是地基：`match` 每调一次就**重新抓一整帧**
//      （"贵的是取帧"），而脚本真正的瓶颈是**按键带宽**（一次 key 只换来一个逻辑帧的按下）；
//      省下的取帧时间要全部还给"多看几眼 + 多按几下"。
//   ④ ⚠ **飞船是线画的**（本体 drawShip = 7 条线 + 座舱）⇒ `matchAll` 回的**连通块**会把
//      飞船拆成好几块，单独哪一块都可能小于尺寸下限而被当噪声丢掉（实测：开局 4.5 秒
//      "看不见自己"，白白按了 R 重开）⇒ **自己必须按"下半场所有 C_SHIP 块的并集"来算**。
//      敌机是 `fillRect` 实心块，所以它反而是干净的一块（旋转后包围盒会变大，属正常）。
//   ⑤ 决策：**排一张"预约表"再选 lane**（见下面 forecast / shootable / 打分循环）。
//      ⚠ 为什么不是"看见最近的威胁就躲"：飞船**恒在一条线上**、敌弹**瞄准发射**、一屏常有
//      5 机 + 5 弹 ⇒ 单威胁反应式必然被逼到角上（实测 16 秒掉光 3 条命、60 分）。这里把未来
//      ~1.5 秒内**每个威胁的(到达时刻, 落点 x, 半径)**摊在 x 轴上打分，挑一条**整段时间都
//      安全**的 lane 走过去 —— "躲开这一发之后站的位置，正是另一发的落点"只有把时间轴排开
//      才看得见。对准开火降级成打分里的一项（顺手能对准的 lane 略加分），不再压过躲闪。
//   ⑥ **道具是分数的杠杆**：`P` 多发（1→5 发平行弹）、`R` 环射（0→4 级，`8+ring*4` 发整圈
//      散射，1 级就 12 发！）、`+` 回血（上限 5 命）。道具**直着落下**（x 不漂）⇒ 对准之后
//      **站住**等它落到我这条线上（拾取判定只有 |dx|<18，晃两下就错过）。
//   ⑦ 阵亡（自己连着好几拍不在画面里）→ 按 R 重开，接着玩；
//   ⑧ 收工：按 `play.json` 的 steps / maxSeconds 停，并写 `session.json`。
//
// ⚠ 按键语义（见 JZComputerUse.h）：`keyDown`/`keyUp` 是**键盘状态** —— 一次调用只投一个"按下"或
//   "抬起"事件，**投递即返回、不阻塞脚本的循环**；"按住多久"由脚本决定，按住就是游戏的原生速度
//   （6px/帧）。`key()` 是"敲一下"（down+up 两次投递），够用于**事件驱动**的消费者（R/回车重开、
//   输入框那种），但**不能**用来"按住"：游戏按逻辑帧轮询按键，两次事件可能落在同一帧之前。
//   ⛔ 本文件以前用 `cu.key(dir)` 重复 N 次当"按住"（一次 16ms 的 Sleep 落在脚本线程上）——
//      那是脚本 loop 频率只有 4.5 次/秒的主因，已全部改成 keyDown/keyUp。
//
// ⚠ 红线（见 prompt.md）：只看得见画面、只操作鼠标键盘；不读容器日志里本体打印的
//   `[shooter] score=..`（那是尺子的特权），也不碰 game/**。
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
    errors: [] as string[],
};

function log(line: string): void {
    const s = '[' + ((Date.now() - t0) / 1000).toFixed(1) + 's] ' + line;
    console.log(s);
    fs.appendFileSync(logPath, s + '\n');
}
const sleep = (ms: number): void => { if (ms > 0) cu.wait(ms); };

// ---------------------------------------------------------------------------
// 题目侧的颜色与几何（与本体 binary/code_agent/runtime/test_jsshooter 一致，见 prompt.md）
// ---------------------------------------------------------------------------
const C_BG        = '#0a0c1e';   // 星空背景（整块客户区 ⇒ 用它校准"游戏区"）
const C_ENEMY_BIG = '#e65a5a';   // 大敌机 46x40（实心 fillRect；3 枪才死，+10 分）
const C_ENEMY_SML = '#dc46a0';   // 小敌机 30x26（实心；1 枪，+10 分）
const C_SHIP      = '#78dcff';   // 自己的飞船（**线画**；受击无敌时整帧隐藏）
const C_EBULLET   = '#ff783c';   // 敌方子弹（朝我飞来的那颗要躲）
const C_PW_MULTI  = '#50a0ff';   // 道具·多发 P：multi 1→5（5 发平行弹）
const C_PW_RING   = '#78e678';   // 道具·环射 R：ring 0→4（8+ring*4 发整圈散射）
const C_PW_LIFE   = '#ffdc5a';   // 道具·回血 +：+1 命（上限 5）

interface Box { x: number; y: number; w: number; h: number; }
interface Blk extends Box { area: number; color: string; }
interface Tgt { box: Box; small: boolean; }

/** 找这个颜色（region = **物理像素**框，缺省整块桌面）；找不到 → null。 */
function matchColor(color: string, region?: Box): Box | null {
    try {
        const loc: { kind: string; color: string; region?: Box } = { kind: 'color', color: color };
        if (region)
            loc.region = region;
        const r = cu.match(JSON.stringify(loc));
        return (r as Box) || null;
    } catch (e) {
        state.errors.push('match 抛错: ' + e);
        return null;
    }
}

/**
 * ⭐ 一次取帧、一遍扫描同时判**全部**关注色，拿回所有连通块（面积降序）。
 * minW/minH 交给 C++ 侧丢噪声 —— 脚本不必再为"这块是不是碎块"多往返一次。
 * ⚠ 下限给得很松（3x3）：飞船是线画的，收紧了下限会把它自己也丢掉（见文件头 ④）。
 */
function lookAll(): Blk[] {
    try {
        const r = cu.matchAll(JSON.stringify({
            colors: [C_SHIP, C_ENEMY_BIG, C_ENEMY_SML, C_PW_RING, C_PW_MULTI, C_PW_LIFE, C_EBULLET],
            all: true, minW: 3, minH: 3, limit: 80,
        }));
        state.looks++;
        return Array.isArray(r) ? (r as Blk[]) : [];
    } catch (e) {
        state.errors.push('matchAll 抛错: ' + e);
        return [];
    }
}

const same = (a: string, b: string): boolean => String(a).toLowerCase() === b.toLowerCase();
const boxCx = (b: Box): number => b.x + b.w / 2;
const boxCy = (b: Box): number => b.y + b.h / 2;
const boxStr = (b: Box | null): string => b
    ? '(' + boxCx(b).toFixed(0) + ',' + boxCy(b).toFixed(0) + ' ' + b.w.toFixed(0) + 'x' + b.h.toFixed(0) + ')'
    : '(没看到)';

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

// ---------------------------------------------------------------------------
// 键盘：**按住 / 抬起**（不是"敲一下再睡一觉"）
// ---------------------------------------------------------------------------
// ⚠ 这里以前是 `cu.key(dir)` 重复 N 次，一次调用 = "按下 → Sleep(16) → 抬起"，而那个 Sleep 落在
//   **脚本自己的线程**上（容器里注入是投递语义，游戏线程不受影响）⇒ 一小节要花 6.6 次 × 31ms
//   ≈ 208ms 在按键上，决策频率被钉在 4.5 次/秒（见 JZComputerUse::keyDown 的说明）。
//   而游戏本来就**按逻辑帧轮询按键**（`keys.has(VK_LEFT)` 才走、`keys.has(VK_SPACE)` 才开火）
//   ⇒ "按住多久"本该由调用方决定。改用 keyDown/keyUp：按键**投递即返回**，按住就是游戏的原生
//   速度（kPlayerSpd 6px/帧 = 375px/s），而且"按住空格"自然会按冷却(8 帧)连续开火。
// ⚠ 收尾必须把按下的键都抬起（否则游戏会一直往一边跑，下一个脚本还继承这个状态）。
let heldDir: '' | 'Left' | 'Right' = '';
let firing = false;

/** 按住某方向（已经按着同一方向则什么都不做）。 */
function hold(dir: 'Left' | 'Right'): void {
    if (heldDir === dir)
        return;
    if (heldDir) { cu.keyUp(heldDir); state.keys++; }
    cu.keyDown(dir);
    heldDir = dir;
    state.keys++;
    state.moves++;
}

/** 停住（对准/站住用：子弹直着上 ⇒ 站住必中）。 */
function halt(): void {
    if (!heldDir)
        return;
    cu.keyUp(heldDir);
    heldDir = '';
    state.keys++;
}

/** 按住空格 = 连续开火（冷却 8 帧 ⇒ 约 7.5 发/秒，比重开一枪一枪按更稳）。 */
function fireOn(): void {
    if (firing)
        return;
    cu.keyDown('Space');
    firing = true;
    state.keys++;
    state.shots++;
}

/** 松开空格。 */
function fireOff(): void {
    if (!firing)
        return;
    cu.keyUp('Space');
    firing = false;
    state.keys++;
}

/** 复位所有按下的键（收尾 / 重开前必须先做）。 */
function releaseAll(): void {
    halt();
    fireOff();
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
        script: 'game/shooter/play.ts', reason, steps: state.steps, slices: state.slices,
        looks: state.looks, moves: state.moves, shots: state.shots, keys: state.keys,
        sawSelf: state.sawSelf,
        sawEnemy: state.sawEnemy, sawBullet: state.sawBullet, sawPw: state.sawPw,
        revives: state.revives, matchMs: state.matchMs, actMs: state.actMs, lookMs: state.lookMs,
        elapsedMs: Date.now() - t0, frames: state.saved, slowSteps: state.slowSteps,
        errors: state.errors.slice(-20), window: cfg.window, cfg, ts: Date.now(),
    };
    try {
        fs.writeFileSync(OUT + '/session.json', JSON.stringify(session, null, 2) + '\n', 'utf8');
    } catch (e) { /* 写不进去就算了，别把脚本带崩 */ }
    releaseAll();                       // ⚠ 收尾复位: 不留"按住不放"的键给下一个脚本/下一局
    log('[shooter-play] 收工: reason=' + reason + ' 拍=' + state.steps + ' 小节=' + state.slices
        + ' 看=' + state.looks + ' 换向=' + state.moves + ' 按键=' + state.keys
        + ' 开火=' + state.shots
        + ' 见自己=' + state.sawSelf + ' 见敌=' + state.sawEnemy + ' 见弹=' + state.sawBullet
        + ' 见道具=' + state.sawPw + ' 重生=' + state.revives + ' 超时拍=' + state.slowSteps
        + (state.errors.length ? (' 噪声/错误=' + state.errors.length) : ''));
}

function main(): void {
    log('[shooter-play] 目标窗口 "' + cfg.window + '"，产物目录 ' + OUT);

    // 等窗口出现，再点一下正中把它**激活**（CU 的单击命中窗口会激活它；之后按键才落在游戏上）。
    // 这个玩法不吃鼠标 ⇒ 这一下没有副作用。
    const end = Date.now() + 15000;
    while (Date.now() < end && !windowReady(cfg.window))
        sleep(300);
    const si = cu.screenInfo ? cu.screenInfo() : null;
    const sw = (si && si.physical ? si.physical[0] : 800) || 800;
    const sh = (si && si.physical ? si.physical[1] : 600) || 600;
    cu.click(sw / 2, sh / 2, 'left');
    // 先按一下 R：**局已经结束就重开**（预热那两秒没人操作，多半已经 GAME OVER 了；
    // 而局还活着时 R 是空操作，见本体 onKeyPress 的 needRecover 判据）⇒ 这一下两个方向都安全，
    // 却省掉了"开局一长串看不见自己"的浪费（实测那一段要 25 小节 ≈ 4.5 秒）。
    cu.key('R');                    // 敲一下: 游戏在 keyPress 里直接处理 R(事件驱动), 不看"按住"
    sleep(60);
    cu.key('Return');
    sleep(200);
    fireOn();                       // 之后一直按住空格 ⇒ 游戏按冷却(8 帧)连续开火

    // 游戏区：窗口刚 show 时客户区还在画（背景色匹配不到成片区域），而命已经在掉了 ⇒
    // **先用整块桌面兜底**（这个玩法的窗口本来就铺满桌面），进了主循环再边打边校准。
    const board: Box = { x: 0, y: 0, w: sw, h: sh };
    const calibrate = (): boolean => {
        const bg = matchColor(C_BG);
        if (!bg || bg.w * bg.h < 100000)      // 整块客户区 ≫ 任何小色块
            return false;
        board.x = bg.x; board.y = bg.y; board.w = bg.w; board.h = bg.h;
        return true;
    };
    let calibrated = calibrate();
    log('[shooter-play] 游戏区 = (' + board.x + ',' + board.y + ' '
        + board.w.toFixed(0) + 'x' + board.h.toFixed(0) + ')'
        + (calibrated ? '' : '（还没画出来，先用整块桌面兜底）'));

    let selfX = board.x + board.w / 2;
    let sawSelfOnce = false;
    let deadSlices = 0;                // 连着多少小节看不到自己（只作日志）
    let lastSelfMs = Date.now();       // ⚠ 阵亡判据用**时间**（见下面 deadLimitMs 的说明）
    let reason = 'steps';
    const wander = { tick: 0 };        // 只剩"追道具的半衰"用（见 pickPowerUp 的 chasing）
    let lastMe: Box | null = null, lastTgt: Box | null = null;
    let lastEbb: Box | null = null, lastPu: Box | null = null;
    let chasing: Box | null = null;    // 正在追的那个道具（它 x 不漂 ⇒ 对准了就别换目标）

    // ⚠ 位置**只由观测决定**：以前是"一次 key = 一个逻辑帧 = 6px"，所以脚本顺手按"键数 × 6"给
    //   selfX 记账；现在"按住 = 连续移动"、位移取决于按住多久 ⇒ 那种记账不成立，位置一律等
    //   下一小节的 matchAll 校正（一小节 ~15ms，够快）。要停就 halt()，游戏下一帧就停。
    // ⏱ 一小节的**节拍**(毫秒) = 观测/决策的频率上限。
    // ⚠ 为什么需要它：按键不再阻塞之后，一小节只剩"取帧 + 扫色"（实测 ~9ms），循环会自己跑到
    //   **~116 次/秒** —— 而每次取帧都是**整块桌面的合成 + 全屏扫色**，跟游戏在**同一个进程**里
    //   抢 CPU（实测那样跑，脚本 116 次/秒、游戏被饿到定格）。50ms(20Hz) 已经比改动前的 4.5Hz
    //   快 4 倍多；游戏逻辑是 60Hz，再快也换不来更好的反应。
    const sliceMs = 50;
    const selfYOf = (): number => board.y + board.h - 60;   // 飞船恒在底部 ~60px

    // ---- 从"一帧里所有块"里挑出各个角色 -------------------------------------
    // ⭐ 自己：飞船是线画的 ⇒ 把下半场**所有** C_SHIP 块并成一个框（见文件头 ④）。
    const pickSelf = (bs: Blk[]): Box | null => {
        let l = 1e9, r = -1e9, t = 1e9, b = -1e9, n = 0;
        for (const x of bs) {
            if (!same(x.color, C_SHIP) || x.area < 8) continue;
            if (boxCy(x) < board.y + board.h * 0.55) continue;   // 只认下半场那块（上半场是残影）
            if (x.x < l) l = x.x;
            if (x.x + x.w > r) r = x.x + x.w;
            if (x.y < t) t = x.y;
            if (x.y + x.h > b) b = x.y + x.h;
            n++;
        }
        return n ? { x: l, y: t, w: r - l, h: b - t } : null;
    };
    // ⚠ **所有**够得着/有威胁的敌机都要拿到（不是只挑一架）：一屏常有 5 架（本体 HUD 的 ENM），
    //   只盯最低那架 ⇒ "躲开了它、撞上了另一架" —— 实测 7 秒掉光 3 条命就是这么来的。
    const pickEnemies = (bs: Blk[], selfY: number): Tgt[] => {
        const out: Tgt[] = [];
        for (const b of bs) {
            const small = same(b.color, C_ENEMY_SML);
            if (!small && !same(b.color, C_ENEMY_BIG)) continue;
            if (b.w < 16 || b.h < 16) continue;                 // 小块当噪声
            if (boxCy(b) > selfY - 20) continue;                // 已经越过我了
            out.push({ box: b, small });
        }
        return out;
    };
    // ================= 轨迹跟踪 + 预约表 =================
    // 为什么不是"看见最近的威胁就躲"：飞船**恒在一条线上**、敌弹**瞄准发射**、一屏常有 5 机 + 5 弹
    //   ⇒ 单威胁反应式必然被逼到角上（实测 16 秒就掉光 3 条命、60 分）。真正要做的是排一张
    //   **未来 ~1.5 秒的预约表**：每个威胁的 (到达时刻 t, 此时落在哪个 x, 半径 r)，然后挑一条
    //   **整段时间都安全**的 lane 走过去 —— "躲开这一发之后站的位置，正是另一发的落点"这件事
    //   只有把时间轴排开才看得见。
    // 速度只从**画面**来：同一实体在两拍之间的位移 ÷ 间隔（不外推游戏内部公式 —— 那是开卷）。
    interface Trk { x: number; y: number; vx: number; vy: number; tMs: number; w: number; h: number; color: string; }
    let trks: Trk[] = [];
    // 逐块与历史轨迹配对（同色 + 预测位置最近），配上就更新速度；没配上当"新出现的实体"。
    const trackAll = (bs: Blk[], nowMs: number): void => {
        const used: boolean[] = [];
        for (let i = 0; i < trks.length; i++) used.push(false);
        const next: Trk[] = [];
        for (const b of bs) {
            const isEnemy = same(b.color, C_ENEMY_BIG) || same(b.color, C_ENEMY_SML);
            if (!isEnemy && !same(b.color, C_EBULLET)) continue;
            if (b.area < 8) continue;
            const cx = boxCx(b), cy = boxCy(b);
            let best = -1, bestD = 1e9;
            for (let i = 0; i < trks.length; i++) {
                if (used[i]) continue;
                const t = trks[i];
                if (!same(t.color, b.color)) continue;
                const dt = (nowMs - t.tMs) / 1000;
                const d = Math.abs(cx - (t.x + t.vx * dt)) + Math.abs(cy - (t.y + t.vy * dt));
                if (d < bestD) { bestD = d; best = i; }
            }
            if (best >= 0 && bestD < 80) {                 // 门限: 别把两个实体串成一条轨迹
                const t = trks[best]; used[best] = true;
                const dt = Math.max(0.02, (nowMs - t.tMs) / 1000);
                const vx = (cx - t.x) / dt, vy = (cy - t.y) / dt;
                // 速度平滑(旧 40% + 新 60%)：单拍位移有量化噪声, 直接外推会抖
                next.push({ x: cx, y: cy, tMs: nowMs, w: b.w, h: b.h, color: b.color,
                            vx: t.vx * 0.4 + vx * 0.6, vy: t.vy * 0.4 + vy * 0.6 });
            } else {
                next.push({ x: cx, y: cy, tMs: nowMs, w: b.w, h: b.h, color: b.color, vx: 0, vy: 0 });
            }
        }
        trks = next;
    };
    const HORIZON_MS = 1200;        // 只看未来这么久；更远的威胁下一拍重新算(那时位置也更准)。
                                    //   ⚠ 别给太长: 一屏常有 10 个威胁, 视界越长"每条 lane 都有账"
                                    //   越严重, 结果就是原地什么都不敢做(实测 1500ms 时纯躲、不打)。
    const SHIP_V = 6 * 60;          // 按住方向键 = 6px/逻辑帧(16ms) ≈ 375px/s —— 用来判"来不来得及"
    interface Threat { tMs: number; x: number; r: number; kind: string; srcX: number;
                       hp: number; killMs: number; }
    /** 预约表: 每个**会穿过我这条线**的威胁 -> (多久后到, 落在哪, 半径多大) */
    const forecast = (selfY: number): Threat[] => {
        const out: Threat[] = [];
        for (const t of trks) {
            if (t.vy < 40) continue;                    // 不往下走的不算威胁(横漂的 / 刚出现还没速度)
            if (t.y > selfY - 6) continue;              // 已经过我头顶
            const sec = (selfY - t.y) / t.vy;
            const tMs = sec * 1000;
            if (tMs > HORIZON_MS) continue;
            // 半径 = 游戏自己的命中盒(弹 16 / 机 w/2+14) + 安全余量 8px —— 目标不是"刚好不被打中",
            //   而是"不擦边"(擦边就没有下一拍的余地了)。
            //   ⚠ 敌机的块是**旋转后**的包围盒(46x40 会报到 ~59x58), 拿 t.w/2 当半宽会算大一半 ⇒
            //   整条 lane 都被判成危险, 于是"到处都不能去"。取**较窄边**再折算回真实宽度。
            const bullet = same(t.color, C_EBULLET);
            const r = (bullet ? 16 : Math.min(t.w, t.h) * 0.6 + 14) + 8;
            // 打掉它要多久: 血 × 开火冷却(8 帧 ≈133ms/发) + 我打上去的子弹飞行时间(660px/s)
            const hp = bullet ? 0 : (same(t.color, C_ENEMY_SML) ? 1 : 3);
            const killMs = hp * 133 + (selfY - t.y) * 1.515;
            out.push({ tMs: Math.max(0, tMs), x: t.x + t.vx * sec, r: r,
                       kind: bullet ? '弹' : '机', srcX: t.x, hp: hp, killMs: killMs });
        }
        return out;
    };
    /** ⭐ **站在 x 上能把它打掉吗** —— 这一条是整个策略的支点。
     *  够条件 = ①它是敌机 ②它到我这线还有 `tMs`，而打死它只要 `killMs`（血×开火冷却 + 子弹飞行），
     *  还得留一点余量 ③我站的位置在它的**射击带**里（±20px，子弹直着上 ⇒ 对准必中）。
     *  满足 ⇒ 它是**收益**（+10 分，而且威胁直接消失），不是威胁 —— 于是"对准开火"和"躲开它"
     *  不再是死冲突：脚本可以理直气壮地站在它的 lane 上把它打掉。
     *  ⚠ 不这么算的后果实测过：只在代价里给它"打个折"，脚本仍然退化成**纯躲**（一局 10 分、
     *    lives 掉光），因为"躲"永远比"冒着被打的风险打它"便宜。 */
    const shootable = (x: number, th: Threat): boolean =>
        th.kind === '机' && th.hp > 0 && th.tMs > th.killMs + 120 && Math.abs(x - th.srcX) <= 20;
    const pickPowerUp = (bs: Blk[], selfY: number): Box | null => {
        let best: Blk | null = null, bestScore = -1e9;
        for (const b of bs) {
            if (b.w < 8 || b.h < 8) continue;
            let worth = 0;
            if (same(b.color, C_PW_RING))       worth = 400;     // 环射：每级 +4 弹，最大的输出杠杆
            else if (same(b.color, C_PW_LIFE))  worth = 260;     // 回血：+1 命 = 多几次机会
            else if (same(b.color, C_PW_MULTI)) worth = 220;     // 多发：5 发平行弹
            else continue;
            const dy = selfY - boxCy(b);
            if (dy < -20) continue;                              // 已经掉过头顶了，追不回来
            const dx = Math.abs(boxCx(b) - selfX);
            if (dx > dy * 1.6 + 60) continue;                    // 够不着（它落下来之前我跑不到那条 x）
            const s = worth - dx * 1.2 + (chasing && Math.abs(boxCx(chasing) - boxCx(b)) < 12 ? 120 : 0);
            if (s > bestScore) { bestScore = s; best = b; }
        }
        return best;
    };
    // 敌弹：**只用于记日志/计数** —— 决策全在预约表(forecast)与 shootable 打的分里。
    //   （旧版这里是"只挑一颗最近且 |dx|<65 的弹来躲"，那正是被逼到角上的原因。）
    const nearestBullet = (bs: Blk[], selfY: number): Box | null => {
        let best: Blk | null = null;
        for (const b of bs) {
            if (!same(b.color, C_EBULLET) || b.w < 3 || b.h < 3) continue;
            if (boxCy(b) > selfY + 10) continue;                 // 已经过我头顶了
            if (!best || boxCy(b) > boxCy(best)) best = b;       // 挑离我最近的那颗
        }
        return best;
    };

    for (let step = 0; ; step++) {
        if (cfg.steps > 0 && step >= cfg.steps) { reason = 'steps'; break; }
        if (cfg.maxSeconds > 0 && (Date.now() - t0) / 1000 >= cfg.maxSeconds) { reason = 'time'; break; }
        const tStep = Date.now();

        if (!calibrated && (step % 3) === 0)
            calibrated = calibrate();

        let slices = 0;
        let act = '';

        // ---- 一拍 = 若干"决策小节"：**看一次（一帧）→ 决策 → 动一下** ----
        while (Date.now() - tStep < cfg.stepMs) {
            const tSlice = Date.now();

            // ⭐ 这一小节的全部观测就这一帧
            const tLook0 = Date.now();
            const bs = lookAll();
            state.lookMs += Date.now() - tLook0;      // ⚠ "看一次"要多久: 它才是节拍的真正下限
            const selfY = selfYOf();

            const me = pickSelf(bs);
            if (me) {
                selfX = boxCx(me); sawSelfOnce = true; state.sawSelf++;
                deadSlices = 0; lastSelfMs = Date.now(); lastMe = me;
            }
            else deadSlices++;

            const es = pickEnemies(bs, selfY);
            let tgt: Tgt | null = null;                       // 用于"对准开火"的那一架（最低的）
            for (const t of es)
                if (!tgt || boxCy(t.box) > boxCy(tgt.box)) tgt = t;
            if (tgt) { state.sawEnemy++; lastTgt = tgt.box; }

            const pu = pickPowerUp(bs, selfY);
            if (pu) { state.sawPw++; lastPu = pu; chasing = pu; }
            else if (chasing && ((wander.tick % 2) === 0)) chasing = null;   // 有一半概率再追一小节

            trackAll(bs, Date.now());      // ⭐ 轨迹：速度只从"两拍之间的位移"估（见 trackAll 的说明）
            const eb = nearestBullet(bs, selfY);
            if (eb) { state.sawBullet++; lastEbb = eb; } else { lastEbb = null; }

            const tAct0 = Date.now();

            // ============ 决策: 排预约表 -> 选一条**整段都安全**的 lane ============
            // 不再"看见最近的威胁就躲": 那条路必然被逼到角上。这里把未来 HORIZON_MS 内的所有
            //   威胁摊在 x 轴上打分, 挑总分最低的落点, 再朝它走(位置由下一拍观测校正)。
            const sideOf = (dx: number): 'Right' | 'Left' => (dx > 0 ? 'Right' : 'Left');
            const ths = forecast(selfY);
            const minX = board.x + 24, maxX = board.x + board.w - 24;
            let bestX = selfX, bestC = 1e18, bestKill = 0;
            for (let x = minX; x <= maxX; x += 8) {
                // 一次遍历同时算"这条 lane 的危险"与"这条 lane 能打掉几架(按急迫度加权)"
                let danger = 0, kill = 0;
                for (const th of ths) {
                    if (shootable(x, th)) {
                        // 越快到我这线越该先打(它已经在排队了); 打掉它 +10 分且**威胁直接消失**
                        kill += 10 + Math.max(0, 800 - th.tMs) / 80;
                        continue;
                    }
                    const gap = Math.abs(x - th.x) - th.r;
                    const near = 1 + 700 / Math.max(120, th.tMs);      // 1.2s 后到 ≈1.6, 0.12s 后到 ≈6.8
                    if (gap <= 0) danger += 100 * near;
                    else if (gap < 26) danger += 3 * near;             // 擦边扣一点(留一拍余地), 别太重
                }
                let c = danger + Math.abs(x - selfX) / 60 - kill;      // 少动 / 能打 = 减分
                if (pu && Math.abs(x - boxCx(pu)) <= 12 && danger <= 0.01) c -= 6;   // 落点安全才去接
                // ⚠ 可达性: 要"躲开某个威胁"才得跑过去的 lane —— 必须在它到达之前跑得到
                let late = 0;
                for (const th of ths) {
                    if (shootable(x, th) || Math.abs(x - th.x) >= th.r) continue;   // 能打掉/不用躲
                    const slack = th.tMs / 1000 - Math.abs(x - selfX) / SHIP_V;
                    if (slack < late) late = slack;
                }
                if (late < 0) c += 60;                                     // 来不及 ⇒ 别去
                if (c < bestC) { bestC = c; bestX = x; bestKill = kill; }
            }
            // ⚠ 应急: 某个威胁 140ms 内就落到我这一格 —— 唯一"来不及规划"的情况(输入还有 ~1 帧延迟),
            //   这时候只做一件事: 朝它来的反方向闪。
            let hurry: Threat | null = null;
            for (const th of ths)
                if (th.tMs < 140 && Math.abs(th.x - selfX) < th.r + 10) hurry = th;
            if (hurry) {
                hold(hurry.x < selfX ? 'Right' : 'Left');
                act = '闪' + hurry.kind + '(' + hurry.tMs.toFixed(0) + 'ms)';
            } else if (Math.abs(bestX - selfX) <= 8) {
                halt();
                act = '占位x=' + bestX.toFixed(0) + '(' + ths.length + '预约/可打' + bestKill.toFixed(0) + ')';
            } else {
                hold(sideOf(bestX - selfX));
                act = '走位→' + bestX.toFixed(0) + '(' + ths.length + '预约/可打' + bestKill.toFixed(0) + ')';
            }

            // ⚠ 看不见自己（受击无敌会整帧隐藏）⇒ **停住**：位置只由观测决定，蒙着走会把"我在哪"搞丢。
            if (!me) {
                halt();
                act += '·看不见自己(停住)';
            }

            // ⚠ 开火**不挑时机**（一直打）：能"稳稳对准"的窗口很少，挑着打反而整局开不了几枪。
            //   按住空格 ⇒ 游戏按冷却(8 帧 ≈133ms)自己连续开火，不用每小节补按键。
            fireOn();

            const sliceUsed = Date.now() - tSlice;     // 这一小节总共花了多久（看 + 动）
            state.slices++;
            state.actMs += Date.now() - tAct0;
            state.matchMs += sliceUsed;
            slices++;

            // 阵亡 → 重开：⚠ 判据必须是**时间**而不是"多少小节" —— 按键不再阻塞之后一小节只 ~15ms，
            //   原来的"12 小节"只剩 0.2 秒，会把"受击无敌期间的隐藏(1.44s)"误判成阵亡，从而疯狂按 R。
            //   所以按"多久没看见自己"判：> 无敌时长即可；还没见过自己时更短（开局那一下已按过 R）。
            const deadLimitMs = sawSelfOnce ? 2000 : 1200;
            const goneMs = Date.now() - lastSelfMs;
            if (goneMs >= deadLimitMs) {
                log('  → 连着 ' + (goneMs / 1000).toFixed(1) + 's 看不到自己（判定阵亡），按 R 重开');
                releaseAll();                    // ⚠ 先复位按键再重开, 别把"按住"带进新一局
                cu.key('R');
                sleep(120);
                cu.key('Return');
                state.revives++;
                deadSlices = 0;
                lastSelfMs = Date.now();
                chasing = null;
                lastTgt = null;
                selfX = board.x + board.w / 2;
                sleep(350);
                fireOn();                        // 新一局继续按住空格
            }

            // ⏱ 按 sliceMs 的节拍走（**下限**，不是"超了就断"）—— 见 sliceMs 的说明。
            //   一拍(cfg.stepMs)里大致跑 cfg.stepMs/sliceMs 个小节。
            const rest = sliceMs - (Date.now() - tSlice);
            if (rest > 0)
                sleep(rest);
        }

        if (step % 5 === 0)
            shot('f' + String(step).padStart(5, '0'));

        log('step ' + step + ' 小节=' + slices
            + ' 看均=' + (state.slices ? (state.lookMs / state.slices).toFixed(0) : '-') + 'ms'
            + ' 我=' + (lastMe ? '(' + selfX.toFixed(0) + ',' + selfYOf().toFixed(0) + ')'
                               : (sawSelfOnce ? '(藏起来了)' : '(还没见过)'))
            + ' 敌=' + boxStr(lastTgt) + ' 弹=' + boxStr(lastEbb) + ' 道具=' + boxStr(lastPu)
            + ' → ' + act + ' 开火=' + state.shots);

        state.steps++;
        const used = Date.now() - tStep;
        if (used > cfg.stepMs)
            state.slowSteps++;
        if (cfg.stepMs > used)
            sleep(cfg.stepMs - used);      // 一拍就是一个 stepMs（尺子按它算预算）
    }
    finish(reason);
}

try {
    main();
} catch (e) {
    log('!! 脚本异常: ' + e);
    finish('crash');
}
