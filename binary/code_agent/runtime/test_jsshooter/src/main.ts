// test_jsshooter —— test/widget/shooter(飞行射击) 的 **TS 版**, 功能逐项对齐 C++ 版:
//   main.cpp / main_window.{h,cpp} -> 本文件末尾的"装配与启动"(JZWindow 800x600 + 铺满的游戏控件)
//   game_widget.{h,cpp}            -> 本文件"仿真"与"绘制"两节(函数名与 C++ 一一对应)
//
// 编译/运行:
//   <tsc> -p tsconfig.json                       (tsgo: <bin>/ts_compiler/tsc/tsc.exe)
//   code_runtime --entry <本目录>/build/main.js   (存在可见顶层窗口, 关窗才退出)
//   类型来自仓库里的 js_wrapper 声明(见 tsconfig.json 的 include); 用户工程里则由
//   initProject 拷进工程的 js_wrapper/**/*.d.ts。
//
// 与 C++ 版的**差异**(受 JS 绑定能力限制, 逐条说明):
//   1) JZFixedTimer/JZTimer 未绑定 -> 逻辑循环用下面的 FixedTimer 类(同一套语义: 墙钟为准、
//      落后连续补帧、单次落后超过 maxCatchUp 就 stop + 报错)。
//   2) 渲染循环: C++ 用 JZTimer(0) = "尽可能快"。JS 的 setInterval 要求间隔 ≥1ms,
//      且宿主定时器唤醒有 ~15ms 粒度, 硬顶达不到"尽可能快"; 所以改成 **排帧 + 丢帧**:
//      每 1ms 试着排一帧, 上一帧还没画完就丢掉这次请求, 于是渲染频率 = 绘制吞吐上限
//      (与 0ms 定时器同效), 且不会把 update() 请求堆起来。
//   3) FPS 统计: C++ 在 onRender(渲染定时器)里统计; 这里在 onPaint 里按**真实绘制帧间隔**
//      统计(FPS/DRAW 都反映实际出帧, 见 updateFps)。
//   4) 绘制统一用颜色字符串(CSS 风格, 支持 '#rrggbbaa' 带 alpha)与 JZFont 的字符串形态
//      (setFont('', size) 等价 C++ 的 JZFont(size) —— 空字体族 = 后端默认微软雅黑)。
//   5) 时间: Date.now()(整数毫秒)代替 std::chrono::steady_clock; 随机数用 Math.random()。
// 其余(实体/概率/加速度/碰撞判定/HUD 文案/绘制顺序与图元数)与 C++ 版完全一致, 便于对拍。
//
// ⚠ 所有权: 顶层窗口归 JS 所有(放手即销毁, 没窗口了 runtime 退出), 所以 win 必须留在
//   模块顶层作用域; 游戏控件经 setParentWidget 挂进窗口树, 由窗口负责 delete + 保活包装。

// `fs` 只给**自动策略的开关**用(读运行根 game.json 的 autoplay) —— 见下面"自动策略"一节。
import fs from 'fs';

// ---------------------------------------------------------------------------
// 常量(对齐 game_widget.h 的 kXxx)
// ---------------------------------------------------------------------------

const kLogicMs = 16;        // 逻辑步长(~60Hz), 仿真速度稳定
// 通关线: 打到这个分数算**通关**(一架 +10 ⇒ 30 架)。与 game/shooter/config.json 的
// targetScore 保持一致 —— 玩法那边按它判分; 本体这边到点了做三件事: 报一行 CLEAR、
// **把这一局停住**(世界定格, 见 onLogic)、画面上盖"恭喜过关"(见 drawClearOverlay)。
const kTargetScore = 300;
const kRenderMs = 1;        // 渲染排帧间隔(C++ 是 0; setInterval 不接受 0)
const kPlayerSpd = 6;
const kBulletSpd = 11;
const kEBulletSpd = 4;
const kMaxEnemy = 140;
const kPi = 3.14159265;

// Win32 虚拟键码(JZKeyEvent.key() 就是平台 VK_*)
const VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28;
const VK_SPACE = 0x20, VK_RETURN = 0x0D;
const VK_A = 0x41, VK_D = 0x44, VK_W = 0x57, VK_S = 0x53, VK_R = 0x52;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

/** [0, n) 的随机整数(对应 std::rand() % n) */
function rnd(n: number): number
{
    return Math.floor(Math.random() * n);
}

/** JZColor(r, g, b[, a]) 的字符串形式: '#rrggbb' / '#rrggbbaa'(画笔画刷的通用颜色形参) */
function col(r: number, g: number, b: number, a: number = 255): string
{
    const h = (v: number): string => v < 16 ? "0" + v.toString(16) : v.toString(16);
    return a >= 255 ? "#" + h(r) + h(g) + h(b) : "#" + h(r) + h(g) + h(b) + h(a);
}

// 每帧重复用到的颜色(避免每帧重新拼串)
const COL_BG        = col(10, 12, 30);
const COL_STAR      = col(190, 200, 255);
const COL_GROUND    = col(40, 60, 90);
const COL_SHIP      = col(120, 220, 255);
const COL_FLAME     = col(255, 165, 0);     // JZColor::Orange
const COL_EBULLET   = col(255, 120, 60);
const COL_BULLET    = col(255, 255, 0);     // JZColor::Yellow
const COL_WHITE     = "#ffffff";
const COL_RED       = "#ff0000";
const COL_HUD_INFO  = col(150, 200, 150);
const COL_HINT      = col(150, 150, 170);
const COL_PW0       = col(80, 160, 255);
const COL_PW1       = col(120, 230, 120);
const COL_PW2       = col(255, 220, 90);
const COL_ENEMY_BIG = col(230, 90, 90);
const COL_ENEMY_SML = col(220, 70, 160);

// ---------------------------------------------------------------------------
// 固定步长定时器(对应 C++ JZFixedTimer): 以墙钟为基准, 唤醒时按真实流逝时间补帧;
// 单次落后超过 maxCatchUp 就认为系统跟不上 -> stop + 报错(防死亡螺旋)。
// JS 侧只有 setInterval, 所以这里自己算"应发射帧数"(等价 C++ 的 behind/interval + 1,
// 但用累计帧数对齐墙钟, 不累积漂移)。
// ---------------------------------------------------------------------------

class FixedTimer
{
    // setInterval 的返回类型: 浏览器里是 number, 容器里(quickjs 的 Node 风格绑定)是 Timeout。
    // 写 ReturnType 两边都对 —— 本工程 strict: true, 写死 number 会编不过。
    private id: ReturnType<typeof setInterval> | null = null;
    private intervalMs = 0;
    // 一次最多补几帧：C++ 的 JZFixedTimer 默认 5；**容器里必须放宽**。
    // 容器侧 setInterval 的唤醒精度远低于 C++ 的高精度计时线程，启动瞬间就会落后 10+ 帧(>5)
    // 而 stop，游戏随即定格(实测: 只打出一次 score 就再无输出)。放宽只影响"一次最多补几帧"，
    // 逻辑步进仍按墙钟对齐，玩法不变。
    private maxCatchUp = 30;
    private startMs = 0;
    private frames = 0;          // 已发射帧数(含补帧)
    private tick: () => void;
    private error: (missed: number) => void;

    constructor(tick: () => void, error: (missed: number) => void)
    {
        this.tick = tick;
        this.error = error;
    }

    start(intervalMs: number): void
    {
        if (this.id !== null)
        {
            console.warn("[test_jsshooter] FixedTimer already started");
            return;
        }
        if (intervalMs <= 0)
        {
            console.warn("[test_jsshooter] FixedTimer interval must be > 0");
            return;
        }
        this.intervalMs = intervalMs;
        this.frames = 0;
        this.startMs = Date.now();
        this.id = setInterval(() => this.timerEvent(), intervalMs);
    }

    stop(): void
    {
        if (this.id !== null)
        {
            clearInterval(this.id);
            this.id = null;
        }
    }

    isActive(): boolean
    {
        return this.id !== null;
    }

    private timerEvent(): void
    {
        const due = Math.floor((Date.now() - this.startMs) / this.intervalMs);   // 到此为止应发射的帧数
        const frames = due - this.frames;
        if (frames <= 0)
            return;      // 底层定时器早触发了一次(引擎侧也会补发): 还没到下一帧

        if (frames > this.maxCatchUp)
        {
            console.warn("[test_jsshooter] FixedTimer: too many frames to catch up (" + frames
                + " > " + this.maxCatchUp + "), stopping");
            this.stop();
            this.error(frames);
            return;
        }

        for (let i = 0; i < frames; i++)
        {
            this.frames++;
            this.tick();
            if (this.id === null)
                return;      // 槽里调用了 stop()
        }
    }
}

// ---------------------------------------------------------------------------
// 游戏控件装配(对照 main_window.cpp)
// ---------------------------------------------------------------------------

// 顶层窗口: 归 JS 所有, 必须留在模块顶层作用域(见文件头的所有权说明)
const win = new JZWindow(800, 600, "JZWidget GDI - 飞行射击");
win.setObjectName("shooterWin");

// 游戏控件: 普通 JZWidget(自绘), 挂进窗口树后由窗口负责生命周期
const game = new JZWidget();
game.setObjectName("game");
game.setParentWidget(win);
game.setGeometry(0, 0, 800, 600);
game.show();

// 窗口尺寸变化 -> 游戏控件铺满客户区(对照 MainWindow::resizeEvent)
win.onResize = () => {
    game.setGeometry(0, 0, win.width(), win.height());
};

// ---------------------------------------------------------------------------
// 游戏状态(对照 GameWidget 的成员)
// ---------------------------------------------------------------------------

interface Star    { x: number; y: number; spd: number; sz: number; }
interface Bullet  { x: number; y: number; vx: number; vy: number; }   // 玩家子弹(环射带 vx/vy)
interface EBullet { x: number; y: number; vx: number; vy: number; }   // 敌方子弹
interface Enemy   { x: number; y: number; w: number; h: number; hp: number; spd: number; type: number; fireCd: number; }
interface PowerUp { x: number; y: number; vy: number; type: number; }  // 0=多发 1=环射 2=回血
interface Boom    { x: number; y: number; life: number; max: number; }

let playerX = 0, playerY = 0;
let playerCd = 0;      // 开火冷却(tick)
let spawnCd = 0;       // 敌人生成冷却(tick)
let invuln = 0;        // 受击无敌帧
let score = 0;
let lives = 3;
let multi = 1;         // 并行弹数量(1..5)
let ring = 0;          // 环射等级(0 关闭, 每级 +4 弹)
let over = false;
let cleared = false;   // 已通关(到达目标分): 世界定格 + 画面上盖"恭喜过关"(见 reportState)
let errMsg = "";       // 非空时表示逻辑落后过多, 游戏已停止

let fps = 0;
let peakFps = 0;
let frameDraws = 0;    // 本帧绘制的图元(绘制请求)数
let peakDraws = 0;
let lastFrameMs = 0;   // 上一帧绘制时刻(FPS 统计)
let animT = 0;         // 动画时钟: 驱动旋转/缩放等矩阵变换(与逻辑步长解耦, 渲染期推进)
let animTickMs = 0;

let stars: Star[] = [];
let bullets: Bullet[] = [];
let ebullets: EBullet[] = [];
let enemies: Enemy[] = [];
let pwups: PowerUp[] = [];
let booms: Boom[] = [];

const keys = new Set<number>();   // 当前按下的虚拟键码

const logicTimer = new FixedTimer(onLogic, onLogicError);

let framePending = false;         // 渲染循环: 已排出一帧、还在等绘制
let renderTimerId: ReturnType<typeof setInterval> | null = null;   // 同上: 容器里是 Timeout

// ---------------------------------------------------------------------------
// 两个循环(对照 GameWidget::onLogic / onRender)
// ---------------------------------------------------------------------------

// 逻辑: 固定步长推进仿真(与渲染解耦)。
// 落后过多(补不过来)时 JZFixedTimer 会先 stop 再发 sigError -> onLogicError。
function onLogic(): void
{
    // 通关后**整幅定格**(见 reportState 的 CLEAR): 背景也不再滚、逻辑步也不再推进 ——
    // 只剩下面"把已有的爆炸播完"和状态打印。重开(R/回车)会把 cleared 清掉、世界重新跑。
    if (!cleared)
        updateStars();             // 背景滚动(放逻辑里, 帧率无关)
    if (!over && !cleared)
        updateGame();

    // 爆炸特效即便 game over 也继续播放完
    for (let i = booms.length - 1; i >= 0; i--)
    {
        booms[i].life--;
        if (booms[i].life <= 0)
            booms.splice(i, 1);
    }

    reportState();                 // 状态打印(见下): 分数变了 / 每 2s / 结束时各一行
}

// ---------------------------------------------------------------------------
// 状态打印 —— **给外面看的出口**（容器把程序 stdout 带 [vproc: "名字"] 前缀转发出去）
//
// 为什么不写状态文件: 本工程是 test/widget/shooter 的 TS 移植(对拍件), 保持"只打印、
// 不碰文件系统"更干净; 想判分的一方从容器 stdout 里读这几行就够 ——
//   [shooter] score=.. lives=..                      分数/命变化的那一刻
//   [shooter] alive score=.. lives=.. over=false     没变化时的心跳(2s 一行, 证明还活着)
//   [shooter] GAME OVER score=..                     游戏结束(一次性)
// ---------------------------------------------------------------------------
let lastReportScore = -1;      // 上一次打印时的分数(-1 = 还没打过)
let lastReportMs = 0;          // 上一次打印的时刻(心跳节流)
let gameOverReported = false;  // GAME OVER 只打一次
let clearReported = false;     // CLEAR 只打一次

function reportState(): void
{
    const now = Date.now();
    if (score !== lastReportScore)
    {
        console.log("[shooter] score=" + score + " lives=" + lives);
        lastReportScore = score;
        lastReportMs = now;
    }
    else if (now - lastReportMs >= 2000)
    {
        console.log("[shooter] alive score=" + score + " lives=" + lives + " over=" + over);
        lastReportMs = now;
    }
    // 通关: 打到目标分(kTargetScore = 300 ⇒ 30 架, 与 game/shooter/config.json 的 targetScore
    //   保持一致)。只报一次。
    // 通关即**停住这一局**: 置 cleared 后 onLogic 不再推进世界(背景也不滚), 玩家/敌机/子弹全
    //   定格在那一刻, paint 那头改画"恭喜过关"; 只有按 R / 回车才会 reset 重开。
    if (!clearReported && score >= kTargetScore)
    {
        clearReported = true;
        cleared = true;        // ⚠ 先定格、再打印: 尺子读到 CLEAR 那行时画面已经是"过关"那一帧
        console.log("[shooter] CLEAR score=" + score + " target=" + kTargetScore);
    }
    if (over && !gameOverReported)
    {
        console.log("[shooter] GAME OVER score=" + score);
        gameOverReported = true;
    }
}

// 逻辑落后过多(补不过来): 停止游戏并上报错误, 不再尝试追帧。
function onLogicError(missed: number): void
{
    console.warn("[test_jsshooter] logic fell too far behind (" + missed + " frames missed), stopping game");
    errMsg = "逻辑帧落后过多, 游戏停止 (落后 " + missed + " 帧)";
    game.update();   // 立即重绘以显示错误遮罩
}

// 渲染: 排一帧。上一帧还没画完就丢掉本次请求 —— 补帧请求在这里被合并成一次重绘,
// 于是帧率 = 绘制吞吐上限(见文件头差异说明 2)。
function onRender(): void
{
    if (framePending)
        return;
    framePending = true;
    game.update();   // 触发重绘
}

// FPS: 按真实绘制帧统计(C++ 在渲染定时器里统计, 见差异说明 3)
function updateFps(): void
{
    const now = Date.now();
    const ms = now - lastFrameMs;
    lastFrameMs = now;

    // 动画时钟(驱动旋转/缩放)。⚠ 通关后**不再推进**: 世界已经定格, 再让飞船/敌机自己转下去,
    // 看着就像"游戏还在跑"(实测第一版就是这样) —— 通关那刻的画面要彻底静下来,
    // 只留 HUD 上那几个统计数字还在变(见 onLogic 的 cleared 说明)。
    const dt = (animTickMs > 0) ? (now - animTickMs) / 1000.0 : 0;
    animTickMs = now;
    if (!cleared)
        animT += dt;

    if (ms > 0)
    {
        const f = 1000.0 / ms;
        fps = (fps === 0) ? f : (fps * 0.9 + f * 0.1);
        if (f > peakFps)
            peakFps = f;
    }
}

function updateStars(): void
{
    const w = game.width(), h = game.height();
    for (let i = 0; i < stars.length; i++)
    {
        const s = stars[i];
        s.y += s.spd;
        if (s.y > h)
        {
            s.y = 0;
            s.x = rnd(w);
        }
    }
}

// ---------------------------------------------------------------------------
// 仿真(对照 GameWidget::updateGame / firePlayer / spawnEnemy / enemyFire / hitPlayer / reset)
// ---------------------------------------------------------------------------

function reset(): void
{
    const w = game.width(), h = game.height();
    playerX = Math.floor(w / 2);
    playerY = h - 60;
    playerCd = 0;
    spawnCd = 0;
    invuln = 0;
    score = 0;
    lives = 3;
    multi = 1;
    ring = 0;
    over = false;
    cleared = false;
    errMsg = "";
    // 状态打印也从这一局重新开始(见 reportState): 否则重开时分数回到 0 也不打一行
    gameOverReported = false;
    clearReported = false;
    lastReportScore = -1;
    lastReportMs = Date.now();
    bullets = [];
    ebullets = [];
    enemies = [];
    pwups = [];
    booms = [];
    keys.clear();

    // 星星背景
    stars = [];
    const n = Math.floor((w * h) / 4000);
    for (let i = 0; i < n; i++)
        stars.push({ x: rnd(w), y: rnd(h), spd: 1 + rnd(2), sz: 1 + rnd(2) });
}

// ---------------------------------------------------------------------------
// 自动策略（自测 / 演示用；**默认关**）
// ---------------------------------------------------------------------------
// 打开方式: **运行根**(`jzApp.applicationDirPath()`)下的 `game.json` 里写 `{"autoplay": true}`。
// ⚠ 为什么既不默认开、也不绑按键: 这个玩法是"只看得见画面"那条任务的本体 —— 默认开或绑个键
//   (比如 F2)就等于把题目送掉(脚本按一下键就必通关)。走自测/演示这条路只能由**外部配置**开,
//   而脚本碰不到 game.json。
// ⚠ 这一节是**开卷**的: 它直接读 enemies/ebullets/pwups 的精确位置与速度(60Hz 决策), 与"看画面
//   猜"的脚本不是一回事 —— 它的用处是①自测这个游戏的难度与平衡②给"能打到多少分"一个上界参考。
let autoplay = false;
let autoTicks = 0;
let autoBudget = 0;             // "规划时间预算用尽"的帧数(诊断: 见 autoplayTick 的时间预算)
// ⚠ 规划用的**位置误差余量**: 策略是按"目标点"规划的, 但真实落点有偏差 —— 目标 x 落在 8px 网格上,
//   而且是每 tick 6px 地靠过去(死区 4px) ⇒ 实际位置与规划位置能差 ±6px。判定盒不留这个余量, 就会
//   出现"规划上还有 1~2px、实际已经压线"的擦边挨打(实测三次掉命全是这种: |dx|=27 vs 29、|dy|=26 vs 27、
//   一颗 dx=14 的弹)。所以规划时把判定盒四边各放大 5px: 用"真实余量"换"绝不压线"。
const AUTO_MG = 5;
// 往上抬行时的"逃跑点"(相对当前位置的偏移): 只试这几个点而不是整行细扫 —— 密集时刻整行细扫会把游戏
// 自己的定步长定时器拖垮(它落后 30 帧就 "too many frames to catch up" 定格整局)。第一个是 0 = 就地抬升。
const AUTO_ESCAPE_X = [0, 32, -32, 64, -64, 96, -96, 128, -128, 160, -160,
                       192, -192, 224, -224, 256, -256, 288, -288];

function initAutoplay(): void
{
    try
    {
        const p = jzApp.applicationDirPath() + '/game.json';
        const cfg = JSON.parse(fs.readFileSync(p, 'utf8')) as { autoplay?: boolean };
        autoplay = (cfg.autoplay === true);
    }
    catch (e)
    {
        autoplay = false;                       // 没有 game.json / 读不动 = 不开（绝不默认开）
    }
    if (autoplay)
        console.log('[autoplay] 自动策略已开启（game.json: {"autoplay": true}）');
}

// ---------------------------------------------------------------------------
// 纵向(y)也参与决策 —— 玩法本来就是 2D 的
// ---------------------------------------------------------------------------
// ⚠ "一维"那件事的落点: 游戏一直支持上下(`VK_UP/VK_DOWN/W/S`, 见 updateGame 的玩家移动与 C++
//   版 game_widget.cpp 的同名代码), 而**之前这版策略只按左右** ⇒ 800x540 的活动区在决策上退化
//   成一条 800px 的横线。后果很具体: "整条横线上总有一发弹", 被逼到墙边(x=16 / w-16)时**一个
//   方向都不能躲** —— 实测三次掉命里有两次就是这么死的(见 [hit] 日志: 我x=25 / 我x=673)。
//   现在把 y 也用上: 优先贴底边打(底边离敌机最远 ⇒ 打掉它的时间最充裕), **当整条底边都被弹幕
//   盖住**时再往上抬去找空隙 —— 纵向有 540px, 比"往左还是往右"多一整个维度的退路。

/** 我在 t(tick, 从"现在"起算) 时某一轴的位置: 从 p0 匀速走到 pEnd, 用 kn 个 tick 走完(之后停住)。
 *  kn <= 0 = 这一轴不动。 */
function playerAt(p0: number, pEnd: number, kn: number, t: number): number
{
    if (kn <= 0 || t >= kn)
        return pEnd;
    return p0 + (pEnd - p0) * (t / kn);
}

/** 威胁 = 一个匀速直线运动的轴对齐判定盒: q(t) = (x0 + vx*t, y0 + vy*t), 盒半宽 rx / 半高 ry。 */
interface Th2
{
    rx: number; ry: number;                                  // 判定盒半宽/半高(已含我的判定盒)
    vx: number; vy: number;                                  // 速度(px/tick)
    x0: number; y0: number;                                  // 现在的位置
    qxlo: number; qxhi: number; qylo: number; qyhi: number;   // 判定窗内它扫过的包围盒(粗筛用)
    t0: number; t1: number;                                  // 可能挨到我的时间窗(已与 [0,HORIZON] 取交)
    enemy: boolean;
    shoot: number;                                           // 我子弹能打中它的半宽(仅敌机)
    hp: number; spd: number;                                 // 仅敌机
}

/** 时间片 [ta,tb] 内, 我这个**点**会不会撞进 th 的判定盒? 会 ⇒ 返回**最早挨上的时刻**, 不会 ⇒ -1。
 *
 *  为什么按"时间片"切: 我的路径只有拐点(两轴各一个: 到点就停), 所以整条路径最多切成 3 片;
 *  **每一片里我和 th 都是匀速直线** ⇒ "2D 盒子进入"有闭式解(精确、O(1)、不分配内存):
 *    单轴上 |f(t)| = |A + B*(t-ta)| < r 的解集是**一个区间**, 两轴取**交**就是"同时满足"。
 *  ⚠ 不能只抽几个时刻比对: 斜着飞的敌弹, "横向在带里"与"纵向在带里"可能发生在**不同**时间,
 *    抽点会把这种擦肩而过误判成"会被挨到"; 而真正挨上的那一下又可能落在抽样点之间。
 *    (1D 版抽 {t0, need, t1} 三点是**精确**的 —— 一维下 f 线性, 极值必在端点; 升到 2D 就不成立了。) */
function sliceHit(px0: number, pendX: number, knX: number,
                  py0: number, pendY: number, knY: number,
                  th: Th2, ta: number, tb: number): number
{
    // 片内我的两轴速度(变向只发生在 knX/knY, 而片边界就是它们 ⇒ 片内是常数)
    const vpx = (knX > ta) ? (pendX - px0) / knX : 0;
    const vpy = (knY > ta) ? (pendY - py0) / knY : 0;
    // 以 ta 为基准: f(u) = 我的位置(ta+u) - 威胁位置(ta+u), u ∈ [0, tb-ta]
    const ax = playerAt(px0, pendX, knX, ta) - (th.x0 + th.vx * ta);
    const ay = playerAt(py0, pendY, knY, ta) - (th.y0 + th.vy * ta);
    const bx = vpx - th.vx, by = vpy - th.vy;
    let xl: number, xh: number;
    if (bx > -1e-9 && bx < 1e-9)                  // 这一轴上相对静止: 要么整片都在带里, 要么整片都不在
    {
        if (ax <= -th.rx || ax >= th.rx)
            return -1;
        xl = -1e9; xh = 1e9;
    }
    else
    {
        const s1 = (-th.rx - ax) / bx, s2 = (th.rx - ax) / bx;
        xl = s1 < s2 ? s1 : s2; xh = s1 < s2 ? s2 : s1;
    }
    let yl: number, yh: number;
    if (by > -1e-9 && by < 1e-9)
    {
        if (ay <= -th.ry || ay >= th.ry)
            return -1;
        yl = -1e9; yh = 1e9;
    }
    else
    {
        const s1 = (-th.ry - ay) / by, s2 = (th.ry - ay) / by;
        yl = s1 < s2 ? s1 : s2; yh = s1 < s2 ? s2 : s1;
    }
    // 两轴取交, 并裁到本片 [0, tb-ta]
    let lo = xl > yl ? xl : yl;
    if (lo < 0) lo = 0;
    let hi = xh < yh ? xh : yh;
    if (hi > tb - ta) hi = tb - ta;
    if (lo > hi) return -1;                       // 这一片里"横向在带里"与"纵向在带里"没有重合(擦肩而过)
    return ta + lo;
}

/** 一个逻辑帧的自动策略: 决定 **往哪挪(x 与 y)** 与 **开不开火**。
 *  调度方式: 直接改 `keys`（就是玩家按键那一套）⇒ 移动速度/边界限位/开火冷却全部复用现成代码。 */
function autoplayTick(): void
{
    // ⚠ **隔帧决策**(32ms 一次): 键是一直按着的, 中间那一帧就照上一拍的决定走 ⇒ 直接省一半开销。
    //   代价是信息最多陈旧 16ms, 而敌弹飞到我这条线要 ~1.4s、敌机落下来要 2~4s, 这点延迟可忽略。
    if ((++autoTicks & 1) === 1)
        return;
    const w = game.width(), h = game.height();
    const minX = 16, maxX = w - 16;
    const minY = 40, maxY = h - 20;                   // 与 updateGame 的限位一致
    const eSpdBase = 2 + Math.floor(score / 160);     // 与 updateGame 里那条一致
    const HORIZON = 100;                              // 只看未来 100 tick(≈1.7s)
    const MAX_ROWS = 6;                               // 最多往上抬 6 行(120px): 圈住搜索开销

    // ---- 收集威胁(2D) ----
    // 判定盒尺度直接取游戏的碰撞代码:
    //   敌弹 vs 我: |dx|<16 且 |dy|<18                    ⇒ 盒 (16, 18)
    //   敌机 vs 我: |dx|<w/2+14 且 |dy|<h/2+14            ⇒ 盒 (w/2+14, h/2+14)
    //   我 vs 敌机: |dx|<w/2+3 (我子弹直着上 ⇒ 只跟 x 有关) ⇒ "命中半宽" shoot
    // ⚠ 撞机盒比命中盒**大 8px** —— 只按"靠近它"去瞄, 就会出现"站在它旁边: 打不着却被撞"的死角。
    // 时间窗 [t0,t1] = "它可能挨到我"的必要条件: 我的活动区是 [minX,maxX]x[minY,maxY], 所以单轴上
    //   它得落进 [lo-r, hi+r] 才有戏; 两轴取交即可(t=0 是现在, 上限 HORIZON)。
    //   (敌弹**上下、横竖都要算**: 敌机越过我之后开的火是 vy<0 从下往上飞; vy≈0 的横向弹会在我
    //    这个高度带上横扫 —— 按"朝我飞"筛会漏掉这两类, 所以这里只按几何算。)
    const ths: Th2[] = [];
    const winMin = (q: number, v: number, r: number, lo: number, hi: number): number =>
        Math.min((lo - r - q) / v, (hi + r - q) / v);
    const winMax = (q: number, v: number, r: number, lo: number, hi: number): number =>
        Math.max((lo - r - q) / v, (hi + r - q) / v);

    for (const b of ebullets)
    {
        const rx = 16 + AUTO_MG, ry = 18 + AUTO_MG;
        let t0 = 0, t1 = HORIZON;
        if (b.vx > 0.001 || b.vx < -0.001)
        {
            t0 = Math.max(t0, winMin(b.x, b.vx, rx, minX, maxX));
            t1 = Math.min(t1, winMax(b.x, b.vx, rx, minX, maxX));
        }
        else if (b.x < minX - rx || b.x > maxX + rx)
            continue;
        if (b.vy > 0.001 || b.vy < -0.001)
        {
            t0 = Math.max(t0, winMin(b.y, b.vy, ry, minY, maxY));
            t1 = Math.min(t1, winMax(b.y, b.vy, ry, minY, maxY));
        }
        else if (b.y < minY - ry || b.y > maxY + ry)
            continue;
        if (t0 > t1) continue;
        const xa = b.x + b.vx * t0, xb = b.x + b.vx * t1;
        const ya = b.y + b.vy * t0, yb = b.y + b.vy * t1;
        ths.push({ rx: rx, ry: ry, vx: b.vx, vy: b.vy, x0: b.x, y0: b.y,
                   qxlo: Math.min(xa, xb) - rx, qxhi: Math.max(xa, xb) + rx,
                   qylo: Math.min(ya, yb) - ry, qyhi: Math.max(ya, yb) + ry,
                   t0: t0, t1: t1, enemy: false, shoot: 0, hp: 0, spd: 0 });
    }
    for (const e of enemies)
    {
        const spd = e.spd + eSpdBase;
        if (spd <= 0) continue;
        const rx = Math.floor(e.w / 2) + 14 + AUTO_MG, ry = Math.floor(e.h / 2) + 14 + AUTO_MG;
        if (e.x < minX - rx || e.x > maxX + rx) continue;          // 敌机不横移(x 恒定)
        const t0 = Math.max(0, winMin(e.y, spd, ry, minY, maxY));
        const t1 = Math.min(HORIZON, winMax(e.y, spd, ry, minY, maxY));
        if (t0 > t1) continue;
        ths.push({ rx: rx, ry: ry, vx: 0, vy: spd, x0: e.x, y0: e.y,
                   qxlo: e.x - rx, qxhi: e.x + rx,
                   qylo: e.y + spd * t0 - ry, qyhi: e.y + spd * t1 + ry,
                   t0: t0, t1: t1, enemy: true,
                   shoot: Math.floor(e.w / 2) + 4, hp: e.hp, spd: spd });
    }

    // ---- 选落脚点 (x,y) ----
    // ⭐ 扫描顺序本身就是策略: **从贴底那一行开始**, 哪一行存在"未来 1.7s 都不会挨到"的 x 就站
    //   那儿(更高的行不用看了); 只有当**整条底边都被弹幕盖住**(连一行干净的都没有)才往上抬 ——
    //   这正是纵向空间该出场的时候。
    //   (贴底优先的道理: 底边离敌机最远 ⇒ tArr 最大 ⇒ 打掉它的时间最充裕、最不容易被撞。)
    let bestX = playerX, bestY = playerY, bestC = 1e18;
    // ⚠ **时间预算**: 这一帧的规划绝不能把游戏自己的定步长定时器拖垮 —— 它是 `FixedTimer`, 落后超过
    //   30 帧就 "too many frames to catch up" 直接把整局停住(实测踩过一次: 威胁 13 个的密集时刻,
    //   分数停在 10)。所以"往上抬行"这件事有个硬上限, 超了就用手上最好的那一行收工。
    const planT0 = Date.now();
    for (let row = 0; row < MAX_ROWS; row++)
    {
        const y = maxY - row * 24;
        if (y < minY) break;
        let cleanX = -1, cleanC = 1e18, anyX = -1, anyC = 1e18;
        // 底边那一行**整行细扫**(8px 网格: 要站得准、要卡在"能打人 / 能接道具"的点上); 上面的行只当
        // **逃跑点**用(见 AUTO_ESCAPE_X —— 从当前位置往两边试几个, 碰到第一个干净的立刻收工)。
        // 密集时刻的开销就压在这两件事上: 逐行细扫会把游戏自己的定步长定时器拖垮。
        for (let xi = 0; ; xi++)
        {
            let x: number;
            if (row === 0)
            {
                x = minX + xi * 8;
                if (x > maxX) break;
            }
            else
            {
                if (xi >= AUTO_ESCAPE_X.length) break;
                x = playerX + AUTO_ESCAPE_X[xi];
                if (x < minX || x > maxX) continue;      // 探到界外(同一点只试一次)
            }
            // ⭐ **路径感知**: 两轴各以 kPlayerSpd/tick 从当前位置走到 (x,y), 拐点在 knX/knY
            //   (这段时间后停住) ⇒ 会不会挨到用**闭式解**一次算完(见 sliceHit), 不逐 tick 扫。
            const knX = Math.abs(x - playerX) / kPlayerSpd;
            const knY = Math.abs(y - playerY) / kPlayerSpd;
            const need = knX > knY ? knX : knY;               // 两轴同时按 ⇒ 到点看较慢那一轴
            let danger = 0, gain = 0;
            for (let i = 0; i < ths.length; i++)
            {
                const th = ths[i];
                // 粗筛: 我这一趟的路径(一条线段) 与 它在判定窗内扫过的包围盒 不相交 ⇒ 一定挨不上。
                // ⚠ 这里是**点**对**盒** —— 判定盒的膨胀(含我的判定盒)已经在 rx/ry 里算过了, 不能再给自己
                //   加一次 ±rx/±ry: 那会把筛子放宽一倍, 密集时白算一大截(实测会把游戏自己的定步长定时器
                //   拖到 "too many frames to catch up" 而整局定格)。
                const myXL = playerX < x ? playerX : x, myXH = playerX < x ? x : playerX;
                if (myXL > th.qxhi || myXH < th.qxlo) continue;
                const myYL = playerY < y ? playerY : y, myYH = playerY < y ? y : playerY;
                if (myYL > th.qyhi || myYH < th.qylo) continue;

                if (th.enemy)
                {
                    // 敌机: 站进它的**命中窗**(shoot, 留 2px 余量)就能打掉它(+10 分, 威胁消失) ——
                    //   但得**来得及**: 到了之后还要有 killT 的时间开火(首发光 + 其余 8 tick/发 +
                    //   子弹飞上去)。⚠ 站在撞机盒里却够不着命中窗 = 打不着却被撞的死角。
                    const tArr = (y - th.y0) / th.spd;         // 它落到我这条 y 上的 tick
                    if (tArr > 0 && Math.abs(x - th.x0) <= th.shoot - 2)
                    {
                        const fly = (y - 18 - th.y0) / (kBulletSpd + th.spd);
                        const killT = (th.hp - 1) * 8 + fly + 4;
                        if (tArr - need > killT + 6)
                        {
                            gain += 10 + Math.max(0, 90 - tArr) / 10;   // 越快到我这高度的越该先打
                            continue;                                   // 能打掉 ⇒ 是收益, 不是威胁
                        }
                    }
                }
                // 精确判定: 路径按两个拐点切成 ≤3 片, 逐片闭式解
                const k1 = knX < knY ? knX : knY, k2 = knX < knY ? knY : knX;
                let hitT = -1;
                const p1 = k1 < th.t1 ? k1 : th.t1;
                if (th.t0 < p1)
                    hitT = sliceHit(playerX, x, knX, playerY, y, knY, th, th.t0, p1);
                if (hitT < 0 && k1 < k2)
                {
                    const a2 = k1 > th.t0 ? k1 : th.t0, b2 = k2 < th.t1 ? k2 : th.t1;
                    if (a2 < b2)
                        hitT = sliceHit(playerX, x, knX, playerY, y, knY, th, a2, b2);
                }
                if (hitT < 0 && k2 < th.t1)
                {
                    const a3 = k2 > th.t0 ? k2 : th.t0;
                    if (a3 < th.t1)
                        hitT = sliceHit(playerX, x, knX, playerY, y, knY, th, a3, th.t1);
                }
                if (hitT >= 0)
                {
                    danger += 100 * (1 + 90 / Math.max(12, hitT));    // 这段时间里会被挨到
                    // ⚠ 这一脚已经踩进去了 ⇒ 剩下的威胁不必再算(只需要知道这个点"脏", 代价多大不重要):
                    //   密集时刻(威胁 10+)这一条就能把开销砍掉一大截, 保住下面的时间预算。
                    if (danger >= 100) break;
                }
                else
                {
                    // 没挨上, 但要留余量: 在窗口两端与两个拐点上量一下"盒外还剩多少"
                    let gap = 1e9;
                    for (let s = 0; s < 4; s++)
                    {
                        const tau = s === 0 ? th.t0 : (s === 1 ? k1 : (s === 2 ? k2 : th.t1));
                        if (tau < th.t0 || tau > th.t1) continue;
                        const dx = Math.abs(playerAt(playerX, x, knX, tau) - (th.x0 + th.vx * tau)) - th.rx;
                        const dy = Math.abs(playerAt(playerY, y, knY, tau) - (th.y0 + th.vy * tau)) - th.ry;
                        const g = dx > dy ? dx : dy;
                        if (g < gap) gap = g;
                    }
                    if (gap < 8)
                        danger += 3 * (1 + 90 / Math.max(12, th.t0));    // 擦边: 扣一点(留一拍的余地)
                }
            }
            // 道具: 只在这点"本来就干净"时顺路接(回血 +1 命 / 环射整圈 / 多发 5 路, 很值钱)
            if (danger <= 0.01)
            {
                for (const u of pwups)
                {
                    if (Math.abs(x - u.x) > 16) continue;             // 拾取判定 |dx|<18
                    const vy = u.vy > 0 ? u.vy : 2;
                    const tU = (y - u.y) / vy;                        // 它落到我这条 y 上的 tick
                    if (tU < 0 || tU > 120 || need > tU) continue;    // 已落过去 / 太久 / 来不及对准
                    const worth = u.type === 2 ? (lives <= 1 ? 26 : 12) : (u.type === 1 ? 14 : 10);
                    gain += worth * (1 - tU / 240);
                }
            }
            let c = danger + Math.abs(x - playerX) / 20 + Math.abs(y - playerY) / 20 - gain;
            // 别贴墙: 墙边横向只剩一个方向(纵向还有, 但横向被截断总归更难躲)
            const edge = Math.min(x - minX, maxX - x);
            if (edge < 40) c += (40 - edge) / 12;
            if (c < anyC) { anyC = c; anyX = x; }
            if (danger <= 0.01)
            {
                if (c < cleanC) { cleanC = c; cleanX = x; }
                if (row !== 0) break;      // 逃跑点: 碰到第一个干净的就不用再往两边试了
            }
        }
        if (cleanX >= 0) { bestX = cleanX; bestY = y; bestC = cleanC; break; }   // 这一行干净 ⇒ 就它
        if (anyC < bestC) { bestC = anyC; bestX = anyX; bestY = y; }             // 都脏: 先记着
        if (Date.now() - planT0 > 3) { autoBudget++; break; }                     // 超预算: 用目前最好的一行
    }

    // 落到 keys 上（按住 = 每 tick 每轴挪 6px；到点就不按 = 站住；4px 死区防抖）
    keys.delete(VK_LEFT); keys.delete(VK_RIGHT); keys.delete(VK_UP); keys.delete(VK_DOWN);
    if (bestX > playerX + 4) keys.add(VK_RIGHT);
    else if (bestX < playerX - 4) keys.add(VK_LEFT);
    if (bestY > playerY + 4) keys.add(VK_DOWN);
    else if (bestY < playerY - 4) keys.add(VK_UP);
    keys.add(VK_SPACE);                                        // 一直开火(8 tick 冷却自己卡住)

    if ((autoTicks % 60) === 0)
        console.log('[autoplay] x=' + Math.round(playerX) + ' y=' + Math.round(playerY)
                    + ' 分=' + score + ' 命=' + lives
                    + ' 敌=' + enemies.length + ' 弹=' + ebullets.length
                    + ' 威胁=' + ths.length
                    + ' 目标=' + Math.round(bestX) + ',' + Math.round(bestY)
                    + (autoBudget > 0 ? (' 超预算=' + autoBudget) : ''));
}

function updateGame(): void
{
    const w = game.width(), h = game.height();
    if (invuln > 0) invuln--;

    // ⭐ 自动策略(默认关): 在"读按键"之前决定往哪走、开不开火 —— 见上面那一节
    if (autoplay && !over && !cleared)
        autoplayTick();

    // 玩家移动
    if (keys.has(VK_LEFT) || keys.has(VK_A)) playerX -= kPlayerSpd;
    if (keys.has(VK_RIGHT) || keys.has(VK_D)) playerX += kPlayerSpd;
    if (keys.has(VK_UP) || keys.has(VK_W)) playerY -= kPlayerSpd;
    if (keys.has(VK_DOWN) || keys.has(VK_S)) playerY += kPlayerSpd;
    playerX = Math.max(16, Math.min(w - 16, playerX));
    playerY = Math.max(40, Math.min(h - 20, playerY));

    // 开火
    if (playerCd > 0) playerCd--;
    if (keys.has(VK_SPACE) && playerCd === 0)
    {
        firePlayer();
        playerCd = 8;
    }

    // 玩家子弹移动
    for (let i = bullets.length - 1; i >= 0; i--)
    {
        const b = bullets[i];
        b.x += b.vx;
        b.y += b.vy;
        if (b.x < -20 || b.x > w + 20 || b.y < -20 || b.y > h + 20)
            bullets.splice(i, 1);
    }
    if (bullets.length > 600)
        bullets.splice(0, bullets.length - 600);

    // 敌方子弹移动
    for (let i = ebullets.length - 1; i >= 0; i--)
    {
        const b = ebullets[i];
        b.x += b.vx;
        b.y += b.vy;
        if (b.x < -20 || b.x > w + 20 || b.y < -20 || b.y > h + 20)
            ebullets.splice(i, 1);
    }

    // 生成敌人(随分数加快, 上限封顶)
    if (spawnCd > 0) spawnCd--;
    const spawnGap = Math.max(10, 46 - Math.floor(score / 40));
    if (spawnCd === 0 && enemies.length < kMaxEnemy)
    {
        spawnEnemy();
        spawnCd = spawnGap;
    }

    // 敌人下移 + 开火
    const eSpdBase = 2 + Math.floor(score / 160);
    for (let i = 0; i < enemies.length; i++)
    {
        const e = enemies[i];
        e.y += e.spd + eSpdBase;
        if (e.fireCd > 0) e.fireCd--;
        if (e.fireCd === 0)
        {
            enemyFire(e);
            e.fireCd = 70 + rnd(90);
        }
    }

    // 掉落道具下移
    for (let i = pwups.length - 1; i >= 0; i--)
    {
        const p = pwups[i];
        p.y += p.vy;
        if (p.y > h + 20)
            pwups.splice(i, 1);
    }

    // 玩家子弹 vs 敌人
    for (let i = enemies.length - 1; i >= 0; i--)
    {
        const en = enemies[i];
        let dead = false;
        for (let j = bullets.length - 1; j >= 0; j--)
        {
            const bt = bullets[j];
            if (Math.abs(bt.x - en.x) < Math.floor(en.w / 2) + 3 &&
                Math.abs(bt.y - en.y) < Math.floor(en.h / 2) + 6)
            {
                en.hp--;
                bullets.splice(j, 1);
                if (en.hp <= 0)
                {
                    score += 10;
                    booms.push({ x: en.x, y: en.y, life: 18, max: 18 });
                    // 掉落道具(约 22%)
                    if (rnd(100) < 22)
                    {
                        const t = rnd(100);
                        const type = t < 40 ? 0 : (t < 75 ? 1 : 2);
                        pwups.push({ x: en.x, y: en.y, vy: 2, type: type });
                    }
                    dead = true;
                }
                break;      // 与 C++ 一致: 一颗子弹只结算一次
            }
        }
        if (dead)
            enemies.splice(i, 1);
    }

    // 敌方子弹 vs 玩家(受击无敌时不计)
    for (let i = ebullets.length - 1; i >= 0; i--)
    {
        const b = ebullets[i];
        if (invuln === 0 && Math.abs(b.x - playerX) < 16 && Math.abs(b.y - playerY) < 18)
        {
            console.log('[hit] 敌弹 x=' + Math.round(b.x) + ' y=' + Math.round(b.y)
                        + ' vx=' + b.vx + ' vy=' + b.vy + ' 我x=' + Math.round(playerX));
            hitPlayer();
            ebullets.splice(i, 1);
        }
    }

    // 敌人 vs 玩家 / 漏底
    for (let i = enemies.length - 1; i >= 0; i--)
    {
        const en = enemies[i];
        if (invuln === 0 &&
            Math.abs(en.x - playerX) < Math.floor(en.w / 2) + 14 &&
            Math.abs(en.y - playerY) < Math.floor(en.h / 2) + 14)
        {
            console.log('[hit] 撞机 x=' + Math.round(en.x) + ' y=' + Math.round(en.y)
                        + ' w=' + en.w + ' h=' + en.h + ' spd=' + (en.spd + eSpdBase)
                        + ' 我x=' + Math.round(playerX));
            hitPlayer();            // 撞机: 扣血
            enemies.splice(i, 1);
        }
        else if (en.y - Math.floor(en.h / 2) > h)
        {
            enemies.splice(i, 1);   // 漏底: 仅移除, 不扣血
        }
    }

    // 道具拾取
    for (let i = pwups.length - 1; i >= 0; i--)
    {
        const u = pwups[i];
        if (Math.abs(u.x - playerX) < 18 && Math.abs(u.y - playerY) < 20)
        {
            if (u.type === 0) multi = Math.min(5, multi + 1);
            else if (u.type === 1) ring = Math.min(4, ring + 1);
            else lives = Math.min(5, lives + 1);
            booms.push({ x: u.x, y: u.y, life: 12, max: 12 });
            pwups.splice(i, 1);
        }
    }
}

function firePlayer(): void
{
    const spd = kBulletSpd;
    // 并行多发(水平展开)
    const n = multi;
    for (let i = 0; i < n; i++)
    {
        const off = Math.floor((i - (n - 1) / 2.0) * 11);
        bullets.push({ x: playerX + off, y: playerY - 18, vx: 0, vy: -spd });
    }
    // 环射: 整圈扩散, 弹数随等级增加
    if (ring > 0)
    {
        const cnt = Math.min(24, 8 + ring * 4);
        for (let i = 0; i < cnt; i++)
        {
            const a = 2.0 * kPi * i / cnt;
            bullets.push({ x: playerX, y: playerY,
                           vx: Math.floor(Math.cos(a) * spd), vy: Math.floor(Math.sin(a) * spd) });
        }
    }
}

function spawnEnemy(): void
{
    const w = game.width();
    const type = rnd(100) < 30 ? 1 : 0;      // 30% 大块头
    const ew = type !== 0 ? 46 : 30;
    const eh = type !== 0 ? 40 : 26;
    enemies.push({ x: 20 + rnd(w - 40), y: -eh,
                   w: ew, h: eh, hp: type !== 0 ? 3 : 1, spd: type !== 0 ? 0 : 1, type: type,
                   fireCd: 60 + rnd(80) });
}

function enemyFire(e: Enemy): void
{
    // 朝玩家方向发射
    const dx = playerX - e.x;
    const dy = playerY - e.y;
    const len = Math.sqrt(dx * dx + dy * dy) + 1e-3;
    ebullets.push({ x: e.x, y: e.y + Math.floor(e.h / 2),
                    vx: Math.floor(dx / len * kEBulletSpd),
                    vy: Math.floor(dy / len * kEBulletSpd) });
}

function hitPlayer(): void
{
    lives--;
    invuln = 90;      // ~2.5s 无敌
    booms.push({ x: playerX, y: playerY, life: 26, max: 26 });
    if (lives <= 0)
    {
        lives = 0;
        over = true;
    }
}

// ---------------------------------------------------------------------------
// 键盘(对照 GameWidget::keyPressEvent / keyReleaseEvent)
// ---------------------------------------------------------------------------

function onKeyPress(e: JZKeyEvent): void
{
    const k = e.key();
    console.log("[key] press " + k + (e.isRepeat() ? " repeat" : ""));
    keys.add(k);
    if (k === VK_R || k === VK_RETURN)
    {
        const needRecover = over || cleared || !logicTimer.isActive();
        if (needRecover)
        {
            reset();
            if (!logicTimer.isActive())
                logicTimer.start(kLogicMs);   // 从"落后停止"状态恢复
        }
    }
    e.accept();
}

function onKeyRelease(e: JZKeyEvent): void
{
    console.log("[key] release " + e.key());
    keys.delete(e.key());
    e.accept();
}

// ---------------------------------------------------------------------------
// 绘制(对照 GameWidget::paintEvent 及各 drawXxx)
// ---------------------------------------------------------------------------

function paint(e: JZPainterEvent): void
{
    void e;                 // 本移植与 C++ 一样整帧重绘(不看脏区)

    updateFps();            // 动画时钟 + FPS 统计(C++ 在 onRender 里做)
    framePending = false;   // 本帧已开始绘制 -> 允许渲染循环再排下一帧
    game.update();          // 续帧: 立即排下一帧(渲染循环的另一半, 见差异说明 2)。
                            // 放在最前: 即使下面的绘制路径抛异常, 渲染循环也不会断。

    const w = game.width(), h = game.height();
    // 框架约定: paintEvent 内自建画笔(裁剪由 painter 构造设定), 事件只提供重绘区域
    const p = new JZPainter(game);
    p.setFont('', 16);      // 等价 C++ 的 JZFont font(16): 空字体族 = 后端默认(微软雅黑)

    frameDraws = 0;

    drawBackground(p, w, h);
    drawExplosions(p);

    // 敌人
    for (let i = 0; i < enemies.length; i++)
    {
        const en = enemies[i];
        drawEnemy(p, en.x, en.y, en.w, en.h, en.type);
    }

    drawBullets(p);
    drawEnemyBullets(p);
    drawPowerUps(p);

    // 玩家(受击无敌时: 隐藏式闪烁 —— 整帧隐藏; 可见帧额外叠加缩放脉冲, 验证矩阵变换)
    const blink = invuln > 0 && (Math.floor(invuln / 5) % 2 === 0);
    if (!over && !blink)
        drawShip(p, playerX, playerY, COL_SHIP, invuln > 0);

    if (frameDraws > peakDraws) peakDraws = frameDraws;

    drawHud(p, w, h);
    if (cleared) drawClearOverlay(p, w, h);
    else if (over) drawOverlay(p, w, h);
    else if (errMsg.length > 0) drawErrorOverlay(p, w, h);

    if (frameDraws > peakDraws) peakDraws = frameDraws;
}

function drawBackground(p: JZPainter, w: number, h: number): void
{
    p.fillRect(0, 0, w, h, COL_BG);

    // 星星向下滚动(位置由 updateStars() 在逻辑步内更新, 此处只读)
    for (let i = 0; i < stars.length; i++)
    {
        const s = stars[i];
        p.fillRect(s.x, s.y, s.sz, s.sz, COL_STAR);
    }

    // 地面线
    p.setPen(COL_GROUND, 1);
    p.drawLine(0, h - 8, w, h - 8);

    frameDraws += 2 + stars.length;   // 背景 + 星星 + 地面线
}

function drawShip(p: JZPainter, cx: number, cy: number, c: string, scalePulse: boolean): void
{
    frameDraws += 8;   // 7 条线 + 1 座舱(save/restore 不计入)
    p.save();
    p.translate(cx, cy);              // 把局部原点移到飞船中心
    p.rotate(animT * 50.0);           // 持续旋转: 验证矩阵 rotate(屏幕顺时针为正)
    if (scalePulse)
    {                                 // 受击期间可见帧: 额外缩放脉冲(变换大小)
        const s = 1.0 + 0.6 * Math.sin(animT * 28.0);
        p.scale(s, s);
    }
    // 以下几何以 (0,0) 为中心绘制
    p.setPen(c, 2);
    p.drawLine(-15, 14, 0, -18);      // 机身(三角)
    p.drawLine(0, -18, 15, 14);
    p.drawLine(15, 14, -15, 14);
    p.drawLine(-15, 14, -24, 4);      // 机翼
    p.drawLine(15, 14, 24, 4);
    const flame = rnd(8) + 4;
    p.setPen(COL_FLAME, 2);
    p.drawLine(-5, 14, -5, 14 + flame);   // 尾焰
    p.drawLine(5, 14, 5, 14 + flame);
    p.setBrush(COL_SHIP);
    p.fillRect(-3, -8, 6, 9, COL_SHIP);   // 座舱
    p.restore();
}

function drawEnemy(p: JZPainter, cx: number, cy: number, w: number, h: number, type: number): void
{
    frameDraws += 4;   // 机身填充 + 描边 + 两只眼(save/restore 不计入)
    // 每敌相位略错开: 旋转 + 轻微缩放脉冲, 验证多实体下的矩阵变换
    const ang = animT * 25.0 + cx * 0.05;
    const sc = 1.0 + 0.12 * Math.sin(animT * 4.0 + cy * 0.03);
    p.save();
    p.translate(cx, cy);
    p.rotate(ang);
    p.scale(sc, sc);

    const hw = Math.floor(w / 2), hh = Math.floor(h / 2);
    const qw = Math.floor(w / 4);
    const body = (type !== 0) ? COL_ENEMY_BIG : COL_ENEMY_SML;
    p.setBrush(body);
    p.fillRect(-hw, -hh, w, h, body);
    p.setPen(col(255, 220, 220), 1);
    p.drawRect(-hw, -hh, w, h);
    // 眼(以中心为基准)
    p.setBrush(COL_WHITE);
    p.fillRect(-qw - 3, -4, 6, 6, COL_WHITE);
    p.fillRect(qw - 3, -4, 6, 6, COL_WHITE);
    p.restore();
}

function drawBullets(p: JZPainter): void
{
    frameDraws += bullets.length;
    p.setBrush(COL_BULLET);
    for (let i = 0; i < bullets.length; i++)
    {
        // 子弹朝向运动方向旋转(环射时呈放射状), 验证大量实体的矩阵变换
        const b = bullets[i];
        const ang = Math.atan2(b.vy, b.vx) * 180.0 / kPi + 90.0;
        p.save();
        p.translate(b.x, b.y);
        p.rotate(ang);
        p.fillRect(-2, -6, 4, 11, COL_BULLET);
        p.restore();
    }
}

function drawEnemyBullets(p: JZPainter): void
{
    frameDraws += ebullets.length;
    p.setBrush(COL_EBULLET);
    for (let i = 0; i < ebullets.length; i++)
    {
        const b = ebullets[i];
        const ang = Math.atan2(b.vy, b.vx) * 180.0 / kPi + 90.0;
        p.save();
        p.translate(b.x, b.y);
        p.rotate(ang);
        p.fillRect(-3, -3, 6, 6, COL_EBULLET);
        p.restore();
    }
}

function drawPowerUps(p: JZPainter): void
{
    frameDraws += 3 * pwups.length;   // 填充 + 描边 + 文字
    for (let i = 0; i < pwups.length; i++)
    {
        const u = pwups[i];
        const c = u.type === 0 ? COL_PW0 : (u.type === 1 ? COL_PW1 : COL_PW2);
        p.setBrush(c);
        p.fillRect(u.x - 9, u.y - 9, 18, 18, c);
        p.setPen("#000000", 1);
        p.drawRect(u.x - 9, u.y - 9, 18, 18);
        p.setPen("#000000", 1);
        const t = u.type === 0 ? "P" : (u.type === 1 ? "R" : "+");
        p.drawText(u.x - 5, u.y - 6, t);
    }
}

// 爆炸: C++ 用 drawArc 画两段圆环(角度单位是**度**: 0 = 3 点方向, 顺时针递增; 0..360 = 整圈), 这里同款
function drawExplosions(p: JZPainter): void
{
    frameDraws += 2 * booms.length;   // 每个爆炸 2 段弧
    for (let i = 0; i < booms.length; i++)
    {
        const b = booms[i];
        const r = b.max - b.life + 4;
        const a = Math.floor(200 * b.life / b.max);
        p.setPen(col(255, 180, 60, a), 2);
        p.drawArc(b.x, b.y, r, 0, 360);
        p.setPen(col(255, 240, 120, Math.floor(a * 0.6)), 2);
        p.drawArc(b.x, b.y, Math.floor(r * 2 / 3), 0, 360);
    }
}

function drawHud(p: JZPainter, w: number, h: number): void
{
    frameDraws += 3 + lives;   // 2 行文字 + 生命图标 + 提示行
    p.setPen(COL_WHITE);
    p.drawText(12, 14, "SCORE:" + score + "/" + kTargetScore
        + "  LIVES:" + lives + "  x" + multi + "  RING:" + ring);

    // 性能信息: FPS(峰值) / 每帧绘制请求数(峰值) / 实体数
    p.setPen(COL_HUD_INFO);
    p.drawText(12, 36,
        "FPS:" + fps.toFixed(0) + "(peak " + peakFps.toFixed(0) + ")" +
        "  DRAW:" + frameDraws + "(peak " + peakDraws + ")" +
        "  ENM:" + enemies.length + "  BUL:" + bullets.length + "  EBUL:" + ebullets.length);

    // 生命小图标
    p.setBrush(COL_SHIP);
    for (let i = 0; i < lives; i++)
        p.fillRect(w - 24 - i * 22, 10, 14, 10, COL_SHIP);

    p.setPen(COL_HINT);
    p.drawText(12, h - 24, "← → / A D 移动   空格 射击   R 重开");
}

function drawOverlay(p: JZPainter, w: number, h: number): void
{
    frameDraws += 4;   // 遮罩 + 3 行文字
    p.fillRect(0, 0, w, h, col(0, 0, 0, 150));
    p.setPen(COL_RED, 3);
    p.setFont('', 48);
    p.drawText((w / 2 - 150) | 0, (h / 2 - 30) | 0, "GAME OVER");
    p.setPen(COL_WHITE, 1);
    p.setFont('', 20);
    p.drawText((w / 2 - 110) | 0, (h / 2 + 20) | 0, "分数: " + score);
    p.drawText((w / 2 - 110) | 0, (h / 2 + 50) | 0, "按 R / 回车 重新开始");
}

// 通关遮罩: 世界此刻已经定格(见 onLogic / reportState), 这里只把"恭喜过关"盖上去。
// 通关不写文件、不额外发事件 —— 日志里还是那行 `[shooter] CLEAR score=.. target=..`(见 reportState)。
function drawClearOverlay(p: JZPainter, w: number, h: number): void
{
    frameDraws += 4;   // 遮罩 + 3 行文字
    p.fillRect(0, 0, w, h, col(0, 0, 0, 160));
    p.setPen(col(120, 255, 120), 3);          // 亮绿: 一眼区别于 GAME OVER 的红
    p.setFont('', 56);
    p.drawText((w / 2 - 112) | 0, (h / 2 - 40) | 0, "恭喜过关");
    p.setPen(COL_WHITE, 1);
    p.setFont('', 22);
    p.drawText((w / 2 - 80) | 0, (h / 2 + 20) | 0, "最终分数: " + score);
    p.setPen(COL_HINT);
    p.setFont('', 18);
    p.drawText((w / 2 - 170) | 0, (h / 2 + 60) | 0, "游戏已停止 —— 按 R / 回车 重新开始");
}

function drawErrorOverlay(p: JZPainter, w: number, h: number): void
{
    p.fillRect(0, 0, w, h, col(0, 0, 0, 170));
    p.setPen(COL_RED, 3);
    p.setFont('', 40);
    p.drawText((w / 2 - 150) | 0, (h / 2 - 40) | 0, "GAME STOPPED");
    p.setPen(col(255, 180, 180), 1);
    p.setFont('', 18);
    p.drawText((w / 2 - 180) | 0, (h / 2 + 10) | 0, errMsg);
    p.setPen(COL_WHITE, 1);
    p.setFont('', 18);
    p.drawText((w / 2 - 130) | 0, (h / 2 + 50) | 0, "按 R / 回车 重新运行");
}

// ---------------------------------------------------------------------------
// 事件挂钩 + 启动(对照 MainWindow 的装配 + main.cpp 的 app.exec)
// ---------------------------------------------------------------------------

game.onPaint = (e: JZPainterEvent) => { paint(e); };
game.onKeyPress = (e: JZKeyEvent) => { onKeyPress(e); };
game.onKeyRelease = (e: JZKeyEvent) => { onKeyRelease(e); };

win.show();          // 首次 show 才创建原生窗口 => 此时客户区尺寸已就绪
game.setFocus();     // 抢占焦点以接收键盘事件(对照 GameWidget::showEvent)
initAutoplay();      // 自动策略开关(默认关; 见上面那一节 reads game.json)
reset();

lastFrameMs = Date.now();
logicTimer.start(kLogicMs);                       // 逻辑: 固定步长
renderTimerId = setInterval(onRender, kRenderMs); // 渲染: 排帧 -> 绘制 -> 再排帧

console.log("[test_jsshooter] started: logic=" + kLogicMs + "ms render=" + kRenderMs + "ms window="
    + win.width() + "x" + win.height()
    + " (方向键/WASD 移动, 空格射击, R 重开)");
