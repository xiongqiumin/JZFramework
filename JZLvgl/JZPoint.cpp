#include "JZPoint.h"

JZPoint::JZPoint()
    : m_x(0), m_y(0)
{
}

JZPoint::JZPoint(int x, int y)
    : m_x(x), m_y(y)
{
}

int JZPoint::x() const   { return m_x; }
int JZPoint::y() const   { return m_y; }
void JZPoint::setX(int x) { m_x = x; }
void JZPoint::setY(int y) { m_y = y; }

void JZPoint::setPoint(int x, int y)
{
    m_x = x; m_y = y;
}

bool JZPoint::isNull() const
{
    return m_x == 0 && m_y == 0;
}

JZPoint JZPoint::operator+(const JZPoint& other) const
{
    return JZPoint(m_x + other.m_x, m_y + other.m_y);
}

JZPoint JZPoint::operator-(const JZPoint& other) const
{
    return JZPoint(m_x - other.m_x, m_y - other.m_y);
}

JZPoint& JZPoint::operator+=(const JZPoint& other)
{
    m_x += other.m_x; m_y += other.m_y;
    return *this;
}

JZPoint& JZPoint::operator-=(const JZPoint& other)
{
    m_x -= other.m_x; m_y -= other.m_y;
    return *this;
}
