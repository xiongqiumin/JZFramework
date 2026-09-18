// test_jsshooter —— test/widget/shooter(飞行射击) 的 **TS 版**, 功能逐项对齐 C++ 版:
//   main.cpp / main_window.{h,cpp} -> 本文件末尾的"装配与启动"(JZWindow 800x600 + 铺满的游戏控件)
//   game_widget.{h,cpp}            -> 本文件"仿真"与"绘制"两节(函数名与 C++ 一一对应)
//
// 编译/运行:
//   <tsc> -p tsconfig.json                       (tsgo: <bin>/ts_compiler/tsc/tsc.exe)
//   code_runtime --root <本目录>/build            (存在可见顶层窗口, 关窗才退出)
//   类型来自仓库里的 js_wrapper 声明(见 tsconfig.json 的 include); 用户工程里则由
//   initProject 拷进工程的 jz_types/**/*.d.ts。
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

// ---------------------------------------------------------------------------
// 常量(对齐 game_widget.h 的 kXxx)
// ---------------------------------------------------------------------------

const kLogicMs = 16;        // 逻辑步长(~60Hz), 仿真速度稳定
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
    private id: number | null = null;
    private intervalMs = 0;
    private maxCatchUp = 5;      // 默认与 JZFixedTimer 一致
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
let renderTimerId: number | null = null;

// ---------------------------------------------------------------------------
// 两个循环(对照 GameWidget::onLogic / onRender)
// ---------------------------------------------------------------------------

// 逻辑: 固定步长推进仿真(与渲染解耦)。
// 落后过多(补不过来)时 JZFixedTimer 会先 stop 再发 sigError -> onLogicError。
function onLogic(): void
{
    updateStars();                 // 背景滚动(放逻辑里, 帧率无关)
    if (!over)
        updateGame();

    // 爆炸特效即便 game over 也继续播放完
    for (let i = booms.length - 1; i >= 0; i--)
    {
        booms[i].life--;
        if (booms[i].life <= 0)
            booms.splice(i, 1);
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

    // 动画时钟(驱动旋转/缩放)
    const dt = (animTickMs > 0) ? (now - animTickMs) / 1000.0 : 0;
    animTickMs = now;
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
    errMsg = "";
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

function updateGame(): void
{
    const w = game.width(), h = game.height();
    if (invuln > 0) invuln--;

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
        const needRecover = over || !logicTimer.isActive();
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
    if (over) drawOverlay(p, w, h);
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
    p.drawText(12, 14, "SCORE:" + score + "  LIVES:" + lives + "  x" + multi + "  RING:" + ring);

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
reset();

lastFrameMs = Date.now();
logicTimer.start(kLogicMs);                       // 逻辑: 固定步长
renderTimerId = setInterval(onRender, kRenderMs); // 渲染: 排帧 -> 绘制 -> 再排帧

console.log("[test_jsshooter] started: logic=" + kLogicMs + "ms render=" + kRenderMs + "ms window="
    + win.width() + "x" + win.height()
    + " (方向键/WASD 移动, 空格射击, R 重开)");
