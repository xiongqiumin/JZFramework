// ============================================================================
// game/shooter/play.ts —— shooter 玩法的**参考脚本**（会真的打：找敌机 → 对准 → 开火）
// ============================================================================
// 它演示的是"**只看画面**打这个飞行射击"的路子（也是 `run_veirfy.py` 默认装的脚本）：
//
//   ① 开局**立刻进场**（游戏从窗口出现那一刻就开始扣命，玩家不动就是挨撞）；
//   ② 校准：`match({kind:'color',color:'#0a0c1e'})` 找星空背景那块**最大连通块** = 游戏区
//      （窗口在桌面上的位置/大小全靠它，不假设原点）；校准前先用整块桌面兜底；
//   ③ 读画面：`match` 一次**只回一个框**（最大连通块），而且画面里同色的小碎块不少
//      （HUD 文字、爆破碎块、道具残影）⇒ 必须**分区 + 限尺寸**：
//        · 自己 = 下半场里那块够大的浅蓝（飞船恒在底部附近）；
//        · 敌机 = 先问下方（逼到跟前的那批），没有再退到整个游戏区；框小于 18x18 的当噪声；
//        · 敌弹 = 下半场那颗橙的（朝我来的才要躲）；
//   ④ **一拍 = 若干"决策小节"**：小节是"看一次 → 决策 → 动一下"的节拍（~200ms）。
//      ⚠ 这是这个玩法的手感核心：游戏逻辑 16ms 一帧、敌机每秒下 187px —— 脚本若是
//        400ms 才眨一次眼、动一次手，等于闭着眼挨撞(实测 3 条命 11 秒见底)。
//   ⑤ **开火只在"对准"的时候**：子弹是直着往上飞的（一次一发、冷却 8 帧），没对准就按
//      等于放空炮 —— 实测帧里子弹密密麻麻打在飞船正上方、敌机在旁边 x 上，一架都打不中。
//   ⑥ 决策：**躲弹 > 让开撞机 > 对准(并对准了才开火) > 游走**；对准之后也**不站定**。
//   ⑦ 阵亡（自己连着好几拍不在画面里）→ 按 R 重开，接着玩；
//   ⑧ 收工：按 `play.json` 的 steps / maxSeconds 停，并写 `session.json`。
//
// ⚠ 按键语义（prompt.md 也写着）：`key()` 是"按下一瞬"——一次 key 只换来**游戏逻辑帧
//   里一帧的按下**（注入侧 press/release 之间隔了 16ms，见 JZComputerUse::key）。
//   要连续移动就得**重复调用**（本文件 move()）。所以"每次调用"都要花掉一个逻辑帧的时间，
//   脚本的移动速度上限就取决于"一小节里能按几次" ⇒ 查画面要省着用（见敌机的隔小节查询）。
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
    steps: 0, slices: 0, moves: 0, shots: 0, sawSelf: 0, sawEnemy: 0, sawBullet: 0,
    revives: 0, saved: 0, slowSteps: 0, matchMs: 0, actMs: 0, errors: [] as string[],
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
const C_ENEMY_BIG = '#e65a5a';   // 大敌机 46x40（3 枪才死，+10 分）
const C_ENEMY_SML = '#dc46a0';   // 小敌机 30x26（1 枪，+10 分）
const C_SHIP      = '#78dcff';   // 自己的飞船（受击无敌时整帧隐藏 ⇒ 那一刻找不到）
const C_EBULLET   = '#ff783c';   // 敌方子弹（朝我飞来的那颗要躲）
const ENEMY_KINDS = [C_ENEMY_BIG, C_ENEMY_SML];

interface Box { x: number; y: number; w: number; h: number; }

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
 * 颜色查找只回**最大连通块**，而同色的小碎块满地都是（HUD 文字、爆破碎块、道具残影）
 * ⇒ 小于尺寸下限的一律当噪声丢掉。这一步很关键：不加它，"敌机"会匹配到 7x7 的子弹残影，
 * 于是脚本朝一个不存在的东西瞄准，一枪都打不中（实测 score 恒为 0）。
 */
function matchBig(color: string, region: Box | undefined, minW: number, minH: number,
                  what: string): Box | null {
    const b = matchColor(color, region);
    if (!b)
        return null;
    if (b.w < minW || b.h < minH) {
        state.errors.push('丢掉噪声 ' + what + ' ' + b.w.toFixed(0) + 'x' + b.h.toFixed(0));
        return null;
    }
    return b;
}

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

/** 移动：一次 key = **按下 → 一帧 → 松开**（见 JZComputerUse::key 的说明）⇒ 重复调用 = 按住。 */
function move(dirKey: string, times: number): void {
    for (let i = 0; i < times; i++) {
        cu.key(dirKey);
        state.moves++;
    }
}

/** 开火：一次 key = 一帧的按下；游戏开火冷却 8 帧(≈133ms)，所以最多一小节一发。 */
function fire(times: number): void {
    for (let i = 0; i < times; i++) {
        cu.key('Space');
        state.shots++;
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
        script: 'game/shooter/play.ts', reason, steps: state.steps, slices: state.slices,
        moves: state.moves, shots: state.shots, sawSelf: state.sawSelf, sawEnemy: state.sawEnemy,
        sawBullet: state.sawBullet, revives: state.revives, matchMs: state.matchMs, actMs: state.actMs,
        elapsedMs: Date.now() - t0, frames: state.saved, slowSteps: state.slowSteps,
        errors: state.errors.slice(-20), window: cfg.window, cfg, ts: Date.now(),
    };
    try {
        fs.writeFileSync(OUT + '/session.json', JSON.stringify(session, null, 2) + '\n', 'utf8');
    } catch (e) { /* 写不进去就算了，别把脚本带崩 */ }
    log('[shooter-play] 收工: reason=' + reason + ' 拍=' + state.steps + ' 小节=' + state.slices
        + ' 移动=' + state.moves + ' 开火=' + state.shots + ' 见敌=' + state.sawEnemy
        + ' 重生=' + state.revives + ' 超时拍=' + state.slowSteps
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
    // ⚠ 别在这儿睡满 warmupSec：**游戏从窗口出现那一刻就开始扣命**（玩家不动就是挨撞），
    //   睡 2 秒 ≈ 白送一条命（实测：等画面等到第 6 秒，3 条命已经掉光）。
    sleep(150);

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

    // 分区（都在"游戏区"里按比例切，坐标是物理像素）。board 会随校准变，所以按需算。
    const band = (y0f: number, hf: number): Box => ({
        x: board.x, y: board.y + Math.floor(board.h * y0f), w: board.w,
        h: Math.floor(board.h * hf),
    });

    let selfX = board.x + board.w / 2;
    let sawSelfOnce = false;
    let deadSlices = 0;                // 连着多少小节看不到自己（判定阵亡）
    let reason = 'steps';
    const wander = { dir: 'Left', tick: 0 };
    let cachedTgt: Box | null = null;  // 敌机（隔小节查一次，中间沿用）
    let missTgt = 0;

    // ⚠ 移动**必须顺手更新 selfX**：颜色查找一小节只做一两次，而移动发生在它之后 ——
    //   不自己记账的话，"我在哪"永远停留在"移动前"，对准判断（|dx| ≤ 容差）就永远不成立，
    //   表现为**一枪都不开**(实测:这么写的一版四局全 0 分)。每次 key 一帧 = 6px，
    //   下一小节的 match 会把它校正回来。
    const moveSelf = (dirKey: string, times: number): void => {
        move(dirKey, times);
        selfX += (dirKey === 'Right' ? 1 : -1) * 6 * times;
        const minX = board.x + 20, maxX = board.x + board.w - 20;   // 游戏把飞船夹在两侧边距内
        if (selfX < minX) selfX = minX;
        if (selfX > maxX) selfX = maxX;
    };
    const sliceMs = Math.max(120, Math.min(220, Math.round(cfg.stepMs * 0.55)));

    for (let step = 0; ; step++) {
        if (cfg.steps > 0 && step >= cfg.steps) { reason = 'steps'; break; }
        if (cfg.maxSeconds > 0 && (Date.now() - t0) / 1000 >= cfg.maxSeconds) { reason = 'time'; break; }
        const tStep = Date.now();

        if (!calibrated && (step % 3) === 0)
            calibrated = calibrate();

        let slices = 0;
        let act = '';
        let lastMe: Box | null = null, lastEbb: Box | null = null, lastTgt: Box | null = null;

        // ---- 一拍 = 若干"决策小节"（看 → 决策 → 动）----
        // ⚠ 每次小节只做 2~3 次颜色查找 + 一组按键（共 ~200ms），所以一拍(400ms)能塞两个小节：
        //   反应速度翻倍。窗口尺寸/分区每次小节重算 —— board 会随校准变。
        while (Date.now() - tStep < cfg.stepMs) {
            const tSlice = Date.now();
            const low = band(0.45, 0.55), upper = band(0.0, 0.9), myBand = band(0.6, 0.4);

            // 看：自己（每小节必查 —— 它是"我在哪"的基准，也是阵亡判据）
            const me = matchBig(C_SHIP, myBand, 8, 8, '自己');
            if (me) { selfX = boxCx(me); sawSelfOnce = true; state.sawSelf++; deadSlices = 0; lastMe = me; }
            else deadSlices++;

            // 看：敌机 —— **隔一小节查一次**（用省下的时间按键：每次 key 要花一个逻辑帧，
            //     查画面多了就挪不动了）。两种颜色 + 两个区域轮着来，兼顾"下方优先"。
            //     连查两次都没有才当它没了（别拿一个早就消失的目标骗自己）。
            if ((slices % 2) === 0) {
                const kind = ENEMY_KINDS[Math.floor(slices / 2) % ENEMY_KINDS.length];
                const near = (slices % 4) < 2;
                const t = matchBig(kind, near ? low : upper, 18, 18, '敌机');
                if (t) { cachedTgt = t; missTgt = 0; state.sawEnemy++; }
                else if (++missTgt >= 2) cachedTgt = null;
            }
            const tgt = cachedTgt;
            if (tgt)
                lastTgt = tgt;

            // 看：敌弹（每 3 小节问一次就够 —— 它飞得快但来得直）
            let eb: Box | null = null;
            if ((slices % 3) === 0) {
                eb = matchBig(C_EBULLET, low, 4, 4, '弹');
                if (eb) { state.sawBullet++; lastEbb = eb; }
            } else {
                eb = lastEbb;
            }

            const selfY = board.y + board.h - 60;    // 飞船恒在底部 ~60px（本脚本不按上下键）
            const tAct0 = Date.now();

            // 决策：躲弹 > 让开撞机 > 对准(对准了才开火) > 游走
            const dodge = eb && boxCy(eb) > selfY - 220 && Math.abs(boxCx(eb) - selfX) < 60;
            const ram = tgt && Math.abs(boxCy(tgt) - selfY) < 130 && Math.abs(boxCx(tgt) - selfX) < 75;
            if (dodge && eb) {
                const dirKey = boxCx(eb) < selfX ? 'Right' : 'Left';
                moveSelf(dirKey, 5);
                act = '躲弹(' + dirKey + ')';
            } else if (ram && tgt) {
                const dirKey = boxCx(tgt) > selfX ? 'Left' : 'Right';
                moveSelf(dirKey, 6);
                act = '让开(' + dirKey + ')';
            } else if (tgt && Math.abs(boxCx(tgt) - selfX) <= 150) {
                // ⚠ 只打"够得着"的目标：按键速率有上限（一次调用 ≈ 一帧），横穿半个屏幕要好几秒，
                //   而敌机 400ms 就下降 75px —— 硬追一个很远的敌机，结果是一路追到被它撞死。
                //   够不着的交给"游走/躲避"：保命比多打一架划算。
                const dx = boxCx(tgt) - selfX;
                if (Math.abs(dx) > 14) {
                    const dirKey = dx > 0 ? 'Right' : 'Left';
                    moveSelf(dirKey, Math.max(1, Math.min(6, Math.round(Math.abs(dx) / 6))));
                    act = '对准' + dirKey;
                } else {
                    // 对准了也**别站定**：敌机/敌弹都是冲着"你当前这一格"来的。左右小幅摆。
                    if ((wander.tick++ % 2) === 0)
                        wander.dir = (wander.dir === 'Left') ? 'Right' : 'Left';
                    moveSelf(wander.dir, 1);
                    act = '对准·摆动';
                }
            } else {
                // 没目标（或目标够不着）也别杵着：游走 + 开火，顺手蹭掉送上门的
                if ((wander.tick++ % 4) === 0)
                    wander.dir = (wander.dir === 'Left') ? 'Right' : 'Left';
                moveSelf(wander.dir, 2);
                act = '游走(' + wander.dir + ')';
            }

            // ⚠ 开火**不挑时机**（一直打）：脚本的按键带宽有限（一次调用 ≈ 一个逻辑帧），能"稳稳
            //   对准"的窗口很少，挑着打反而整局开不了几枪（实测挑时机那版 4 局 0~10 分，一直打
            //   的版本 4 局 20~60 分）。子弹便宜、命中主要靠"敌机自己撞进弹道"，所以一拍接一拍
            //   地按 —— 游戏冷却 8 帧(≈133ms)，一小节一发刚好不浪费。
            fire(1);

            const sliceUsed = Date.now() - tSlice;     // 这一小节总共花了多久（看 + 动）
            state.slices++;
            state.actMs += Date.now() - tAct0;
            state.matchMs += sliceUsed;
            slices++;

            // 阵亡 → 重开：受击无敌只隐藏 2 秒（≈10 小节），所以"见过自己之后"连着 14 小节
            // 看不到就是命掉光了。⚠ 还没见过自己时要放宽（开局画面刚出来那几秒本来就看不见，
            // 按 14 小节判会把一局好游戏按 R 打断 —— 实测发生过），但也不能无限等（否则一上来
            // 就是 GAME OVER 的画面时会一直空转）。
            const deadLimit = sawSelfOnce ? 14 : 25;
            if (deadSlices >= deadLimit) {
                log('  → 连着 ' + deadSlices + ' 小节看不到自己（判定阵亡），按 R 重开');
                cu.key('R');
                sleep(120);
                cu.key('Return');
                state.revives++;
                deadSlices = 0;
                cachedTgt = null;
                selfX = board.x + board.w / 2;
                sleep(400);
            }

            if (sliceUsed >= sliceMs)
                break;                                  // 这一小节够长了：交给下一拍
        }

        if (step % 5 === 0)
            shot('f' + String(step).padStart(5, '0'));

        log('step ' + step + ' 小节=' + slices
            + ' 我=' + (lastMe ? '(' + selfX.toFixed(0) + ',' + (board.y + board.h - 60).toFixed(0) + ')'
                               : (sawSelfOnce ? '(藏起来了)' : '(还没见过)'))
            + ' 敌=' + boxStr(lastTgt) + ' 弹=' + boxStr(lastEbb) + ' → ' + act
            + ' 开火=' + state.shots);

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
