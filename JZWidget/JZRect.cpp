#include "JZRect.h"
#include "JZDebug.h"
#include <algorithm>

JZRect::JZRect()
    : m_x(0), m_y(0), m_w(0), m_h(0)
{
}

JZRect::JZRect(int x, int y, int w, int h)
    : m_x(x), m_y(y), m_w(w), m_h(h)
{
}

void JZRect::clear()
{
    m_x = 0; 
    m_y = 0;
    m_w = 0;
    m_h = 0;
}

int JZRect::x() const      { return m_x; }
int JZRect::y() const      { return m_y; }
int JZRect::width() const  { return m_w; }
int JZRect::height() const { return m_h; }

void JZRect::setX(int x)       { m_x = x; }
void JZRect::setY(int y)       { m_y = y; }
void JZRect::setWidth(int w)   { m_w = w; }
void JZRect::setHeight(int h)  { m_h = h; }

void JZRect::setRect(int x, int y, int w, int h)
{
    m_x = x; m_y = y; m_w = w; m_h = h;
}

// ==================== 边 ====================

int JZRect::left() const   { return m_x; }
int JZRect::top() const    { return m_y; }
int JZRect::right() const  { return m_x + m_w - 1; }
int JZRect::bottom() const { return m_y + m_h - 1; }

// ==================== 角 ====================

JZPoint JZRect::topLeft() const     { return JZPoint(left(), top()); }
JZPoint JZRect::topRight() const    { return JZPoint(right(), top()); }
JZPoint JZRect::bottomLeft() const  { return JZPoint(left(), bottom()); }
JZPoint JZRect::bottomRight() const { return JZPoint(right(), bottom()); }

// ==================== 中心 ====================

JZPoint JZRect::center() const
{
    return JZPoint(m_x + m_w / 2, m_y + m_h / 2);
}

// ==================== 尺寸 ====================

JZSize JZRect::size() const
{
    return JZSize(m_w, m_h);
}

// ==================== 判断 ====================

bool JZRect::contains(int px, int py) const
{
    return px >= m_x && px <= m_x + m_w && py >= m_y && py <= m_y + m_h;
}

bool JZRect::contains(const JZPoint& p) const
{
    return contains(p.x(), p.y());
}

bool JZRect::isEmpty() const
{
    return m_w <= 0 || m_h <= 0;
}

// ==================== 几何运算 ====================

JZRect JZRect::intersect(const JZRect& o) const
{
    int x1 = std::max(left(),   o.left());
    int y1 = std::max(top(),    o.top());
    int x2 = std::min(right(),  o.right());
    int y2 = std::min(bottom(), o.bottom());
    if (x2 < x1 || y2 < y1)
        return JZRect();
    return JZRect(x1, y1, x2 - x1 + 1, y2 - y1 + 1);
}

JZRect JZRect::united(const JZRect& o) const
{
    if (isEmpty()) return o;
    if (o.isEmpty()) return *this;
    int x1 = std::min(left(),   o.left());
    int y1 = std::min(top(),    o.top());
    int x2 = std::max(right(),  o.right());
    int y2 = std::max(bottom(), o.bottom());
    return JZRect(x1, y1, x2 - x1 + 1, y2 - y1 + 1);
}

bool JZRect::intersects(const JZRect& o) const
{
    return !intersect(o).isEmpty();
}

JZDebug operator<<(JZDebug d, const JZRect &rc)
{
    d.append(str_asprintf("{%d,%d,%d,%d}",rc.x(),rc.y(),rc.width(),rc.height()).c_str());
    return d;
}
