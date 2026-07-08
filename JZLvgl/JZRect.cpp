#include "JZRect.h"

JZRect::JZRect()
    : m_x(0), m_y(0), m_w(0), m_h(0)
{
}

JZRect::JZRect(int x, int y, int w, int h)
    : m_x(x), m_y(y), m_w(w), m_h(h)
{
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

JZSize JZRect::size() const
{
    return JZSize(m_w, m_h);
}

bool JZRect::contains(int px, int py) const
{
    return px >= m_x && px <= m_x + m_w && py >= m_y && py <= m_y + m_h;
}

bool JZRect::isEmpty() const
{
    return m_w <= 0 || m_h <= 0;
}
