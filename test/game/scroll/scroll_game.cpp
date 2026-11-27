#include "scroll_game.h"

#include "JZApplication.h"
#include "JZWindow.h"
#include "JZImage.h"
#include "JZFont.h"
#include "JZPen.h"
#include "JZBrush.h"
#include "JZKeyEvent.h"

#include <cmath>
#include <cstdio>

// Win32 虚拟键码 (JZKeyEvent::key 即平台 VK_*)
enum {
    VK_LEFT_  = 0x25,
    VK_UP_    = 0x26,
    VK_RIGHT_ = 0x27,
    VK_DOWN_  = 0x28,
    VK_R       = 0x52,
    VK_PLUS   = 0xBB,   // '='
    VK_MINUS  = 0xBD,   // '-'
};

// ---------------------------------------------------------------- 纹理
// 远景山: 256x200, 上半透明 —— 横向平铺无缝 (两端的山跨越接缝)。
// 只铺一行 (drawTiledRow), 不铺满整屏: 一张 256x200 的图 4 次 drawPixmap 就够,
// 比"整屏平铺"省一个数量级的填充面积 (200% DPI 下尤其明显)。
static JZPixmap makeHillsTexture()
{
    const int w = 256, h = 200;
    JZImage img = JZImage::create(w, h);
    {
        JZPainter p(img);
        // 中心的远峰 + 两端各半座 (平铺后与下一块拼接)
        p.fillEllipse(128 - 110, h - 190, 220, 220, JZColor(30, 44, 72));
        p.fillEllipse(0 - 80, h - 140, 160, 160, JZColor(36, 52, 84));
        p.fillEllipse(w - 80, h - 140, 160, 160, JZColor(36, 52, 84));
    }
    return JZPixmap::fromImage(img);
}

// 金币 16x16: 外圈金 + 内圈高光
static JZPixmap makeCoinTexture()
{
    JZImage img = JZImage::create(16, 16);
    {
        JZPainter p(img);
        p.fillEllipse(1, 1, 14, 14, JZColor(250, 205, 70));
        p.fillEllipse(5, 5, 6, 6, JZColor(255, 245, 180));
    }
    return JZPixmap::fromImage(img);
}

// ---------------------------------------------------------------- 控件
ScrollGame::ScrollGame(JZWidget* parent)
    : JZGameView2D(parent)
{
    makeTextures();

    // 渲染锁 60FPS: JZGameView 的默认渲染间隔是 0 ("尽可能快")。本示例在 200% DPI 下
    // 整窗重绘 (1920x1080 的 GDI+ 软件渲染 + 大块半透明山), 无限制重绘会把主线程占满
    // —— 实测 Debug 只有 9FPS 且手感发滞。固定 16ms 后逻辑/渲染都是 60Hz, 稳定且省 CPU。
    setRenderInterval(16);
}

void ScrollGame::makeTextures()
{
    m_texHills = makeHillsTexture();
}

void ScrollGame::spawnCoins()
{
    m_coins.clear();
    const JZPixmap coin = makeCoinTexture();
    for (int i = 0; i < (int)kCoinCount; i++) {
        JZSprite& s = m_coins.create<JZSprite>();
        s.texture = coin;          // 共享同一张 pixmap (引用计数, 不拷贝像素)
        s.width = 16;
        s.height = 16;
        s.anchorX = 0.5;
        s.anchorY = 0.5;
        s.x = 240.0 + i * 56.0;    // 固定铺开: 自检时可见数可预期
        s.y = 250.0 + (i % 5) * 30.0;
        s.z = 0;
    }
    m_total = (int)m_coins.size();
}

void ScrollGame::resetGame()
{
    // 世界尺寸一设, 相机就再也不会滚出边界 (右边界 = kWorldW)
    camera().setWorldSize(kWorldW, kWorldH);
    camera().setZoom(1.0);
    camera().setFollowRate(8.0);   // 每秒收敛 8 次 ≈ 跟手但不僵硬

    m_playerX = 120.0;
    m_step = 0;
    m_nextReport = 90;
    m_done = false;

    spawnCoins();
    camera().snapTo(m_playerX + 140.0, kPlayerY - 60.0);
    report();
}

void ScrollGame::stepGame()
{
    if (m_done)
        return;

    m_step++;

    if (m_auto) {
        m_playerX += m_speed;                    // 自动模式: 一直往右跑
    } else {
        double vx = 0.0;
        if (isKeyDown(VK_LEFT_))  vx -= m_speed;
        if (isKeyDown(VK_RIGHT_)) vx += m_speed;
        m_playerX += vx;
    }
    if (m_playerX < 40.0)              m_playerX = 40.0;
    if (m_playerX > kWorldW - 40.0)    m_playerX = kWorldW - 40.0;

    // 镜头看向角色**前方** 140px (= 横版滚屏的"预看"), 垂直略偏上留出地面
    followCamera(m_playerX + 140.0, kPlayerY - 60.0);

    if (m_step >= m_nextReport) {
        report();
        m_nextReport += 100;
    }

    // 抓图 (被遮挡/最小化也能抓到自绘内容; 现场看画面/agent 自查都靠它)
    if (!m_shotPath.empty() && !m_shotDone && m_step >= 240) {
        JZImage img = grab();
        const bool ok = !img.isNull() && img.save(m_shotPath);
        std::printf("[scroll] shot %s -> %s (%dx%d)\n", ok ? "ok" : "FAIL",
                    m_shotPath.c_str(), img.width(), img.height());
        std::fflush(stdout);
        m_shotDone = true;
    }

    // 自动模式: 跑够 520 步 (3120px) 后相机应被右边界钳住
    if (m_auto && m_step >= 520)
        finish();
}

void ScrollGame::report()
{
    // 现算一次可见数: 这一步可能还没画过 (reset 时 report), 用上一帧的 m_drawn 会印 0。
    m_drawn = (int)m_coins.countVisible(visibleWorld());
    std::printf("[scroll] step=%d playerX=%.0f camX=%.1f zoom=%.2f visible=%d/%d\n",
                m_step, m_playerX, camera().x(), camera().zoom(), m_drawn, m_total);
    std::fflush(stdout);
}

void ScrollGame::finish()
{
    m_done = true;

    // 相机的右钳制位置 = worldW - 视口宽/2 (相机存的是"视口中心")
    const double expect = kWorldW - width() * 0.5;
    const bool ok = std::fabs(camera().x() - expect) < 1.0;

    std::printf("[scroll] DONE step=%d camX=%.1f expect=%.1f clamp=%s visible=%d/%d\n",
                m_step, camera().x(), expect, ok ? "ok" : "FAIL", m_drawn, m_total);
    std::fflush(stdout);

    // 收尾走**关窗**这条正路 (close() -> WM_CLOSE -> notifyWindowClosed -> quit):
    // 直接 jzApp->quit() 会让 exec 在"窗口还开着"的状态下返回, 随后栈上的 JZWindow/控件
    // 带着活的顶层窗口析构, 实测偶发退出时 0xC0000005。
    if (JZWindow* w = window())
        w->close();
    else if (jzApp)
        jzApp->quit();
}

// ---------------------------------------------------------------- 绘制
// 背景层: 屏幕坐标 (相机还没施加)。视差就靠 factor 区分。
void ScrollGame::renderBackground(JZPainter* p)
{
    const int vw = width();
    const int vh = height();

    // 夜空 + 月亮 (钉在屏幕上: 不随相机移动)
    p->fillRect(0, 0, vw, vh, JZColor(14, 20, 34));
    p->fillEllipse(vw - 130, 40, 64, 64, JZColor(226, 230, 205));

    // 远景山: factor 0.25 —— 相机动 100px, 山只动 25px; 山脚贴地平线
    drawTiledRow(p, m_texHills, 0.25, (int)kGroundY - m_texHills.height());
}

// 地面: 世界层的东西就画在世界层 (坐标为世界坐标, 相机自然带上滚屏速度)。
// 早先这里是"屏幕空间平铺 + setClip 裁出地平线以下" —— JZPainter 的 save/restore
// **不保存裁剪**, 忘掉恢复就会把后面的世界/HUD 全裁掉 (实测只剩背景)。这类"就是世界的一部分"
// 的东西直接放 renderWorld 最省事, 也不用手动算平铺偏移。
void ScrollGame::drawGround(JZPainter* p, const JZBody& view)
{
    const int gy = (int)kGroundY;
    const double x0 = std::floor(view.x / 80.0) * 80.0;      // 对齐到 80 的网格, 滚动不抖
    const double x1 = view.x + view.w;
    if (x1 <= x0)
        return;

    const int w = (int)(x1 - x0);
    p->fillRect((int)x0, gy, w, (int)(kWorldH - kGroundY), JZColor(58, 44, 34));
    p->fillRect((int)x0, gy, w, 8, JZColor(70, 120, 70));
    p->fillRect((int)x0, gy + 8, w, 2, JZColor(46, 90, 52));

    p->setPen(JZPen(JZColor(48, 36, 28), 1));
    for (double x = x0; x <= x1; x += 80)
        p->drawLine((int)x, gy + 10, (int)x, (int)kWorldH);
    p->drawLine((int)x0, gy + 34, (int)x1, gy + 34);
}

// 世界层: 这里的坐标全是世界坐标 (painter 已施加相机)
void ScrollGame::renderWorld(JZPainter* p)
{
    const JZBody view = visibleWorld();
    drawGround(p, view);
    m_drawn = (int)m_coins.countVisible(view);
    m_coins.drawVisible(p, view);          // 只把屏幕内的金币写进绘制流

    // 终点旗 (世界坐标固定, 用来目测相机钳制是否准)
    p->fillRect((int)kWorldW - 130, (int)kGroundY - 120, 6, 120, JZColor(200, 200, 210));
    p->setPen(JZPen(JZColor(240, 120, 120), 3));
    p->drawLine((int)kWorldW - 124, (int)kGroundY - 120, (int)kWorldW - 70, (int)kGroundY - 108);
    p->drawLine((int)kWorldW - 70, (int)kGroundY - 108, (int)kWorldW - 124, (int)kGroundY - 96);

    // 角色
    const int px = (int)m_playerX;
    const int py = (int)kPlayerY;
    // ⚠ drawRoundRect 会**按当前 brush 填内部**: brush 不设就是黑的, 把 fillRoundRect
    //   刚填的蓝色盖掉 (踩过)。要么先 setBrush, 要么只用 fillRoundRect。
    p->setBrush(JZBrush(JZColor(90, 200, 255)));
    p->setPen(JZPen(JZColor(16, 40, 62), 2));
    p->drawRoundRect(px - 14, py - 18, 28, 36, 9);
    p->fillEllipse(px + 2, py - 10, 7, 7, JZColor(255, 255, 255));
    p->fillEllipse(px + 4, py - 8, 4, 4, JZColor(20, 20, 30));

    // 脚下影子
    p->fillEllipse(px - 16, py + 16, 32, 8, JZColor(0, 0, 0, 90));
}

// HUD 层: 屏幕坐标 (相机已还原)
void ScrollGame::renderHud(JZPainter* p)
{
    char buf[192];

    std::snprintf(buf, sizeof(buf),
                  "WORLD %.0fx%.0f   CAM (%.0f, %.0f)   ZOOM %.2f   COINS on screen %d/%d",
                  kWorldW, kWorldH, camera().x(), camera().y(), camera().zoom(),
                  m_drawn, m_total);
    p->setPen(JZPen(JZColor::White));
    p->drawText(12, 14, buf);

    std::snprintf(buf, sizeof(buf), "FPS %.0f (peak %.0f)   step %d   %s",
                  fps(), peakFps(), m_step,
                  m_auto ? "AUTO" : "Left/Right move, +/- zoom, R restart");
    p->setPen(JZPen(JZColor(150, 200, 150)));
    p->drawText(12, 34, buf);

    // 滚屏进度条: 相机中心从"左钳制位"到"右钳制位"的归一化位置
    const int barX = 12, barY = 58, barW = 320, barH = 6;
    const double half = width() * 0.5;
    const double maxCam = kWorldW - half;
    p->fillRect(barX, barY, barW, barH, JZColor(40, 50, 70));

    if (maxCam > half + 0.5) {
        double t = (camera().x() - half) / (maxCam - half);
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        const int done = (int)(barW * t);
        p->fillRect(barX, barY, done, barH, JZColor(90, 200, 255));
        p->fillRect(barX + done - 1, barY - 3, 3, barH + 6, JZColor::White);
    }

    // 左下角: 相机可见世界矩形的边角 (证明 visibleWorld 与 renderWorld 用的是同一套数)
    const JZBody v = visibleWorld();
    std::snprintf(buf, sizeof(buf), "view world [%.0f, %.0f] .. [%.0f, %.0f]",
                  v.x, v.y, v.x + v.w, v.y + v.h);
    p->setPen(JZPen(JZColor(120, 130, 160)));
    p->drawText(12, 74, buf);
}

void ScrollGame::keyPressEvent(JZKeyEvent* event)
{
    const uint32_t k = event->key();
    if (k == VK_PLUS) {
        camera().setZoom(camera().zoom() * 1.25);    // 放大 -> 可见范围变小
    } else if (k == VK_MINUS) {
        camera().setZoom(camera().zoom() / 1.25);
    } else if (k == VK_R) {
        resetGame();
    }
    JZGameView2D::keyPressEvent(event);              // 交给基类登记按键/accept
}
