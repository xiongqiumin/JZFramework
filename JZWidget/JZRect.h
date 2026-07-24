#ifndef JZ_RECT_H_
#define JZ_RECT_H_

#include "JZPoint.h"
#include "JZSize.h"

class JZDebug;
class JZRect
{
public:
    JZRect();
    JZRect(int x, int y, int w, int h);

    int x() const;
    int y() const;
    int width() const;
    int height() const;

    void setX(int x);
    void setY(int y);
    void setWidth(int w);
    void setHeight(int h);

    void setRect(int x, int y, int w, int h);

    // 边
    int left() const;
    int top() const;
    int right() const;
    int bottom() const;

    // 角
    JZPoint topLeft() const;
    JZPoint topRight() const;
    JZPoint bottomLeft() const;
    JZPoint bottomRight() const;

    // 中心
    JZPoint center() const;

    JZSize size() const;

    bool contains(int px, int py) const;
    bool contains(const JZPoint& p) const;
    bool isEmpty() const;

    // 几何运算: 交集(无交返回空矩形) / 并集外接框 / 是否相交
    JZRect intersect(const JZRect& other) const;
    JZRect united(const JZRect& other) const;
    bool intersects(const JZRect& other) const;

private:
    int m_x = 0;
    int m_y = 0;
    int m_w = 0;
    int m_h = 0;
};
JZDebug operator<<(JZDebug d, const JZRect &rc);

#endif // JZ_RECT_H_
