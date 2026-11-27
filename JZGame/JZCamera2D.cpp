#include "JZCamera2D.h"
#include "JZPainter.h"

#include <algorithm>

// 视口 = (世界 - 中心) * zoom + 视口中心。
// 三个动作都必须重算钳制: 换视口尺寸、换 zoom、换世界边界 —— 它们都会改变
// "视口在世界里占多大", 少算一次就会露出边界外的空白。

void JZCamera2D::setViewportSize(double w, double h)
{
    m_viewW = w > 0 ? w : 0;
    m_viewH = h > 0 ? h : 0;
    clamp();
}

void JZCamera2D::setPosition(double x, double y)
{
    m_x = x;
    m_y = y;
    clamp();
}

void JZCamera2D::move(double dx, double dy)
{
    m_x += dx;
    m_y += dy;
    clamp();
}

double JZCamera2D::left() const
{
    return m_x - m_viewW / (2.0 * m_zoom);
}

double JZCamera2D::top() const
{
    return m_y - m_viewH / (2.0 * m_zoom);
}

void JZCamera2D::setZoom(double z)
{
    // 下界 0.05 (看到 20 倍范围): 再小就没意义, 还会让 visibleRect 大到剔除失效;
    // 上界 20: GDI 后端在大倍率下笔宽/文本会糊, 留个理性上限。
    if (!(z > 0.0))
        z = 1.0;
    m_zoom = std::min(std::max(z, 0.05), 20.0);
    clamp();
}

void JZCamera2D::setWorldBounds(const JZBody& bounds)
{
    if (bounds.w <= 0 || bounds.h <= 0) {
        clearWorldBounds();
        return;
    }
    m_bounds = bounds;
    m_hasBounds = true;
    clamp();
}

void JZCamera2D::setWorldSize(double w, double h)
{
    setWorldBounds(JZBody(0, 0, w, h));
}

void JZCamera2D::clearWorldBounds()
{
    m_hasBounds = false;
    m_bounds = JZBody();
}

void JZCamera2D::setFollowRate(double perSecond)
{
    m_followRate = perSecond;
}

void JZCamera2D::follow(double targetX, double targetY, double dtSeconds)
{
    if (!(dtSeconds > 0.0)) {
        // 没有时间推进就不动 (渲染帧里调 follow 的写法也安全)
        return;
    }
    if (m_followRate <= 0.0) {
        snapTo(targetX, targetY);
        return;
    }

    // 指数收敛: 与步长/帧率无关 (dt 加倍, 剩余误差平方级下降)
    const double alpha = 1.0 - std::exp(-m_followRate * dtSeconds);
    m_x += (targetX - m_x) * alpha;
    m_y += (targetY - m_y) * alpha;
    clamp();
}

void JZCamera2D::snapTo(double targetX, double targetY)
{
    setPosition(targetX, targetY);
}

// ---------------------------------------------------------------- 坐标换算
// 与 applyTo 的变换严格互逆 (zoom 只做等比缩放)。
void JZCamera2D::worldToScreen(double wx, double wy, double& sx, double& sy) const
{
    sx = (wx - m_x) * m_zoom + m_viewW * 0.5;
    sy = (wy - m_y) * m_zoom + m_viewH * 0.5;
}

void JZCamera2D::screenToWorld(double sx, double sy, double& wx, double& wy) const
{
    wx = (sx - m_viewW * 0.5) / m_zoom + m_x;
    wy = (sy - m_viewH * 0.5) / m_zoom + m_y;
}

double JZCamera2D::worldToScreenX(double wx) const
{
    return (wx - m_x) * m_zoom + m_viewW * 0.5;
}

double JZCamera2D::worldToScreenY(double wy) const
{
    return (wy - m_y) * m_zoom + m_viewH * 0.5;
}

double JZCamera2D::screenToWorldX(double sx) const
{
    return (sx - m_viewW * 0.5) / m_zoom + m_x;
}

double JZCamera2D::screenToWorldY(double sy) const
{
    return (sy - m_viewH * 0.5) / m_zoom + m_y;
}

// ---------------------------------------------------------------- 可见区
JZBody JZCamera2D::visibleRect() const
{
    const double w = m_viewW / m_zoom;
    const double h = m_viewH / m_zoom;
    return JZBody(left(), top(), w, h);
}

bool JZCamera2D::isVisible(const JZBody& worldBody, double pad) const
{
    if (m_viewW <= 0 || m_viewH <= 0)
        return false;   // 还没拿到视口尺寸: 一律不画 (首帧/隐藏时)

    JZBody v = visibleRect();
    if (pad != 0.0) {
        v.x -= pad;
        v.y -= pad;
        v.w += pad * 2.0;
        v.h += pad * 2.0;
    }
    return v.intersects(worldBody);
}

void JZCamera2D::applyTo(JZPainter* painter) const
{
    if (!painter)
        return;
    // 顺序 = T(视口中心) * S(zoom) * T(-相机中心): 之后提交的几何都是世界坐标。
    painter->translate(m_viewW * 0.5, m_viewH * 0.5);
    painter->scale(m_zoom, m_zoom);
    painter->translate(-m_x, -m_y);
}

// ---------------------------------------------------------------- 钳制
void JZCamera2D::clamp()
{
    if (!m_hasBounds)
        return;

    const double halfW = m_viewW / (2.0 * m_zoom);
    const double halfH = m_viewH / (2.0 * m_zoom);

    // 世界比视口窄/矮的那个轴: 固定居中 (否则 clamp 会把 lo>hi 抖成边界值)
    if (halfW * 2.0 >= m_bounds.w)
        m_x = m_bounds.centerX();
    else
        m_x = std::min(std::max(m_x, m_bounds.x + halfW), m_bounds.x + m_bounds.w - halfW);

    if (halfH * 2.0 >= m_bounds.h)
        m_y = m_bounds.centerY();
    else
        m_y = std::min(std::max(m_y, m_bounds.y + halfH), m_bounds.y + m_bounds.h - halfH);
}
