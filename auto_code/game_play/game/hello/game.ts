// ==================== 游戏机 hello.ts（点球玩法；题目侧，不是被测实现） ====================
// 规则（人话版）：
//   · 屏幕上随机出现 **红球** 和 **蓝球**（纯色 + 黑边，好认）；
//   · **点红球 +1 分，点蓝球 -1 分**（鼠标点，命中 = 落在圆内）；
//   · **初始 5 分，到 10 分通关**（CLEAR!），**掉到 0 分就输**（GAME OVER）；
//   · 球被点中 / 到期会在别处重生（换个位置、换个颜色）—— 所以画面一直在动；
//   · 按 R / 回车重开。
//
// 为什么有它：比 shooter 简单得多 —— 只有"看颜色 + 点位置"两件事，用来练
// "视觉 agent"的闭环（截图 → 找红/蓝 → 点）。参数（窗口/通关线/初始分/球数…）都可配。
//
// ⚠ 它是**题目侧**（被测 App）：脚本只能"看这块屏、点这个鼠标"。改这里 = 换题。
// ⚠ 状态上报（score/over/win）是给**尺子(verify.py)**读的，脚本按红线不许读它。
import fs from 'fs';

const kRunDir = jzApp.applicationDirPath();
let kStatusPath = kRunDir + '/hello_status.json';
let kTargetScore = 10;      // 通关线（到它就 CLEAR）
let kStartScore = 5;        // 初始分
try {
    const cfg = JSON.parse(fs.readFileSync(kRunDir + '/game.json', 'utf8'));
    if (typeof cfg.targetScore === 'number' && cfg.targetScore > 0)
        kTargetScore = cfg.targetScore;
    if (typeof cfg.startScore === 'number' && cfg.startScore > 0)
        kStartScore = cfg.startScore;
    if (typeof cfg.statusPath === 'string' && cfg.statusPath.length > 0)
        kStatusPath = cfg.statusPath;
} catch (e) { /* 没有 game.json 就用缺省 */ }

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------
const kW = 480, kH = 360;      // 窗口/客户区尺寸（与 config.json 的 native 一致）
const kBallR = 34;             // 球半径（大一点，好点）
const kBallNum = 3;            // 同时几个球
const kBallLifeMs = 4000;      // 球活多久就换地方（让画面一直动，也逼脚本别背位置）
const kHudH = 64;              // 顶部 HUD 高度（球不会摆到这里）

const kBg = '#f2f2f2';
const kRed = '#e02020';
const kBlue = '#2050e0';
const kInk = '#202020';

const VK_R = 0x52, VK_RETURN = 0x0D;

interface Ball { x: number; y: number; red: boolean; bornMs: number; }

let balls: Ball[] = [];
let score = 0;
let over = false;        // 掉到 0 分
let cleared = false;     // 到通关线
let hits = 0;            // 点中次数
let misses = 0;          // 点空次数
let lastClick = '';      // 最近一次点击的描述（诊断用）

function rnd(n: number): number { return Math.floor(Math.random() * n); }

/** 下一个球该是什么颜色：保证场上**至少一红一蓝**（纯随机可能全红或全蓝，不好练）。 */
function nextColor(): boolean
{
    let reds = 0;
    for (const b of balls)
        if (b.red) reds++;
    if (reds === 0) return true;                 // 一个红的都没有 -> 必须红
    if (reds === balls.length) return false;     // 全是红的 -> 必须蓝
    return rnd(2) === 0;
}

/** 随机摆一个球：躲开 HUD 与边缘，尽量不和现有球重叠。 */
function placeBall(red: boolean): Ball
{
    const xMin = kBallR + 12, yMin = kHudH + kBallR + 8;
    const xSpan = Math.max(1, kW - 2 * (kBallR + 12));
    const ySpan = Math.max(1, kH - (yMin + kBallR + 12));
    for (let attempt = 0; attempt < 40; attempt++)
    {
        const x = xMin + rnd(xSpan);
        const y = yMin + rnd(ySpan);
        let ok = true;
        for (const b of balls)
        {
            const dx = b.x - x, dy = b.y - y;
            const minGap = 2 * kBallR + 18;
            if (dx * dx + dy * dy < minGap * minGap) { ok = false; break; }
        }
        if (ok)
            return { x: x, y: y, red: red, bornMs: Date.now() };
    }
    // 实在挤不下：随便给个位置（宁可重叠，也别卡住）
    return { x: xMin + rnd(xSpan), y: yMin + rnd(ySpan), red: red, bornMs: Date.now() };
}

function reset(): void
{
    balls = [];
    balls.push(placeBall(true));                       // 开局保证一红一蓝
    balls.push(placeBall(false));
    while (balls.length < kBallNum)
        balls.push(placeBall(nextColor()));
    score = kStartScore;
    over = false;
    cleared = false;
    hits = 0;
    misses = 0;
    lastClick = '';
}

// ---------------------------------------------------------------------------
// 窗口 + 控件（对照 shooter 的装配）
// ---------------------------------------------------------------------------
const win = new JZWindow(kW, kH, "HELLO");   // 标题 = 脚本认窗口的名字（与 config.json 的 window 一致）
win.setObjectName("helloWin");

const game = new JZWidget();      // 普通 JZWidget(自绘)，挂进窗口树后由窗口负责生命周期
game.setObjectName("hello");
game.setParentWidget(win);
game.setGeometry(0, 0, kW, kH);
game.show();

win.onResize = () => { game.setGeometry(0, 0, win.width(), win.height()); };

// ---------------------------------------------------------------------------
// 鼠标：点球（唯一操作）
// ---------------------------------------------------------------------------
function onClick(x: number, y: number): void
{
    if (over || cleared)
        return;
    for (let i = 0; i < balls.length; i++)
    {
        const b = balls[i];
        const dx = x - b.x, dy = y - b.y;
        if (dx * dx + dy * dy <= kBallR * kBallR)      // 命中（点在圆内）
        {
            score += b.red ? 1 : -1;
            hits++;
            lastClick = "(" + x + "," + y + ") " + (b.red ? "红 +1" : "蓝 -1") + " -> " + score;
            balls[i] = placeBall(nextColor());         // 点中的球换个地方/颜色重生
            if (score >= kTargetScore)
                cleared = true;
            else if (score <= 0)
                over = true;
            game.update();
            return;
        }
    }
    misses++;
    lastClick = "(" + x + "," + y + ") 点空";
}

game.onMousePress = (e: JZMouseEvent) => { onClick(e.localX(), e.localY()); };

// 键盘只有一个作用：重开（R / 回车），对照其它玩法的习惯
game.onKeyPress = (e: JZKeyEvent) => {
    const k = e.key();
    if ((k === VK_R || k === VK_RETURN) && (over || cleared))
    {
        reset();
        writeStatus();
        game.update();
    }
};

// ---------------------------------------------------------------------------
// 逻辑：球到期换地方（没有别的规则 —— 所以这个玩法简单）
// ---------------------------------------------------------------------------
function onLogic(): void
{
    if (over || cleared)
        return;
    const now = Date.now();
    for (let i = 0; i < balls.length; i++)
        if (now - balls[i].bornMs > kBallLifeMs)
            balls[i] = placeBall(nextColor());
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
function paint(e: JZPainterEvent): void
{
    void e;
    framePending = false;          // 本帧已开始绘制 -> 允许渲染循环再排下一帧
    game.update();                 // 续帧（放最前：下面即使抛异常，渲染循环也不断）

    const p = new JZPainter(game);
    const w = game.width(), h = game.height();

    p.setBrush(kBg);
    p.fillRect(0, 0, w, h, kBg);

    // 球
    for (const b of balls)
    {
        const c = b.red ? kRed : kBlue;
        p.setBrush(c);
        p.fillEllipse(b.x - kBallR, b.y - kBallR, kBallR * 2, kBallR * 2, c);
        p.setPen(kInk, 2);
        p.drawEllipse(b.x - kBallR, b.y - kBallR, kBallR * 2, kBallR * 2);
    }

    // HUD
    p.setPen(kInk, 2);
    p.setFont('', 26);
    p.drawText(14, 40, "分数 " + score + " / " + kTargetScore);
    p.setFont('', 14);
    p.drawText(14, 58, "点红球 +1，点蓝球 -1（" + kStartScore + " 分开始，"
                       + kTargetScore + " 分通关，0 分输）");

    // 结束/通关遮罩
    if (cleared || over)
    {
        p.setPen(cleared ? '#1a8f3c' : '#b00000', 4);
        p.setFont('', 44);
        p.drawText((w / 2 - 110) | 0, (h / 2) | 0, cleared ? "CLEAR!" : "GAME OVER");
        p.setPen(kInk, 2);
        p.setFont('', 16);
        p.drawText((w / 2 - 80) | 0, (h / 2 + 44) | 0, "按 R / 回车 重开");
    }
}

// ---------------------------------------------------------------------------
// 渲染排帧（与 shooter 同一套：上一帧没画完就丢掉本次请求，帧率 = 绘制吞吐上限）
// ---------------------------------------------------------------------------
let framePending = false;

function onRender(): void
{
    if (framePending)
        return;
    framePending = true;
    game.update();
}

// ---------------------------------------------------------------------------
// 状态上报（**尺子读它**，脚本按红线不许读它）
// ---------------------------------------------------------------------------
function writeStatus(): void
{
    try
    {
        fs.writeFileSync(kStatusPath, JSON.stringify({
            play: 'hello', score: score, over: over, win: cleared,
            target: kTargetScore, start: kStartScore, hits: hits, misses: misses,
            lastClick: lastClick, running: !over && !cleared,
            w: win.width(), h: win.height(), ts: Date.now(),
        }) + '\n', 'utf8');
    }
    catch (e) { /* 写不进去就算了，别把游戏带崩 */ }
}

// ---------------------------------------------------------------------------
// 起转
// ---------------------------------------------------------------------------
game.onPaint = (e: JZPainterEvent) => { paint(e); };

win.show();          // 首次 show 才创建原生窗口 => 此时客户区尺寸已就绪
game.setFocus();     // 键盘（R 重开）要焦点；鼠标事件不需要
reset();

setInterval(onLogic, 100);        // 逻辑：只有"球到期换地方"，100ms 粒度足够
setInterval(onRender, 8);         // 渲染排帧（~120Hz 上限）
setInterval(writeStatus, 300);    // 状态落盘
writeStatus();

console.log("[hello] started: window=" + win.width() + "x" + win.height()
    + " target=" + kTargetScore + " start=" + kStartScore + " balls=" + kBallNum
    + " (点红球 +1, 点蓝球 -1, R 重开)");
