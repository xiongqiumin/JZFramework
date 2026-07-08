#ifndef JZ_RECT_H_
#define JZ_RECT_H_

#include "JZSize.h"

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
    JZSize size() const;

    bool contains(int px, int py) const;
    bool isEmpty() const;

private:
    int m_x = 0;
    int m_y = 0;
    int m_w = 0;
    int m_h = 0;
};

#endif // JZ_RECT_H_
