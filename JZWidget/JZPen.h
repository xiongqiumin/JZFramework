#ifndef JZ_PEN_H_
#define JZ_PEN_H_
#include "JZColor.h"

class JZFRAMEWORK_API JZPen
{
public:
    JZPen() = default;
    JZPen(const JZColor& color, int width = 1);

    void setColor(const JZColor& color);
    JZColor color() const;

    void setWidth(int width);
    int width() const;

private:
    JZColor m_color;
    int m_width = 1;
};

#endif // JZ_PEN_H_
