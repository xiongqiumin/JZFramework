#include "JZGameView2D.h"
#include "JZPainter.h"
#include "JZPixmap.h"

#include <cmath>

JZGameView2D::JZGameView2D(JZWidget* parent)
    : JZGameView(parent)
{
    // 视口尺寸要等控件有大小; renderGame 每帧兜一次 (见 syncViewport),
    // 这里只是让"构造后立刻查 visibleWorld()"不至于返回零矩形。
    syncViewport();
}

void JZGameView2D::syncViewport()
{
    const double w = width();
    const double h = height();
    if (w != m_camera.viewportWidth() || h != m_camera.viewportHeight())
        m_camera.setViewportSize(w, h);
}

void JZGameView2D::followCamera(double targetX, double targetY)
{
    // 用逻辑步长当 dt: 逻辑 timer 是固定步长, 所以跟随速度不受渲染帧率影响。
    // (落后补帧时本函数会被连续调用多次, 每次 dt 都是同一个步长 —— 总位移仍然正确)
    m_camera.follow(targetX, targetY, logicInterval() / 1000.0);
}

JZBody JZGameView2D::visibleWorld() const
{
    if (!m_cameraEnabled)
        return JZBody();
    return m_camera.visibleRect();
}

bool JZGameView2D::isVisible(const JZBody& worldBody, double pad) const
{
    if (!m_cameraEnabled)
        return true;   // 相机不接管时不做剔除 (坐标空间由子类自己决定)
    return m_camera.isVisible(worldBody, pad);
}

void JZGameView2D::drawTiledBackground(JZPainter* p, const JZPixmap& tex,
                                       double factorX, double factorY)
{
    if (!p || tex.isNull())
        return;

    const int tw = tex.width();
    const int th = tex.height();
    const int vw = width();
    const int vh = height();
    if (tw <= 0 || th <= 0 || vw <= 0 || vh <= 0)
        return;

    // 起点 = -(相机左上角 * factor) 对瓦片尺寸取模: 相机动多远, 背景动多远(factor 倍)。
    // 与相机用同一个 left()/top(), 所以 factor=1 时"地砖"和世界物体严丝合缝。
    double ox = std::fmod(m_camera.left() * factorX, (double)tw);
    double oy = std::fmod(m_camera.top() * factorY, (double)th);
    if (ox > 0.0) ox -= tw;   // fmod 的符号跟着被除数, 统一到 (-tw, 0]
    if (oy > 0.0) oy -= th;

    // 只铺覆盖视口的那几块 (vw/tw * vh/th 次 drawPixmap)
    for (int y = (int)oy; y < vh; y += th) {
        for (int x = (int)ox; x < vw; x += tw)
            p->drawPixmap(tex, x, y, tw, th);
    }
}

void JZGameView2D::drawTiledRow(JZPainter* p, const JZPixmap& tex, double factorX, int y)
{
    if (!p || tex.isNull())
        return;

    const int tw = tex.width();
    const int th = tex.height();
    const int vw = width();
    if (tw <= 0 || th <= 0 || vw <= 0)
        return;

    double ox = std::fmod(m_camera.left() * factorX, (double)tw);
    if (ox > 0.0)
        ox -= tw;

    for (int x = (int)ox; x < vw; x += tw)
        p->drawPixmap(tex, x, y, tw, th);
}

void JZGameView2D::renderGame(JZPainter* p)
{
    syncViewport();

    renderBackground(p);     // 屏幕坐标 (视差/天空/平铺地砖)

    // ⚠ JZPainter 的 save/restore 只保存 pen/brush/font/矩阵 —— **裁剪不在其中**。
    //   背景层常有"只在地平线以下铺地砖"这种需求 (setClip 一块区域再画), 画完不重置就会
    //   把后面的世界层/HUD 一起裁掉 (实测: 只剩背景, 角色和文字全消失)。
    //   这里统一把裁剪恢复成整个控件 (游戏的每帧本来就是整块重绘, 丢掉脏区细化无所谓)。
    p->setClip(JZRect(0, 0, width(), height()));

    if (m_cameraEnabled) {
        p->save();
        m_camera.applyTo(p);
        renderWorld(p);      // 世界坐标
        p->restore();
    } else {
        renderWorld(p);      // 未变换 (子类自己管坐标)
    }

    // 世界层里若临时改过裁剪, 同样别带进 HUD
    p->setClip(JZRect(0, 0, width(), height()));

    renderHud(p);            // 屏幕坐标
}

void JZGameView2D::resizeEvent(JZResizeEvent* event)
{
    JZWidget::resizeEvent(event);
    syncViewport();
    update();                // 视口变了, 立即按新尺寸重画一帧
}
