#ifndef JZ_COLOR_H_
#define JZ_COLOR_H_
#include "JZGlobal.h"
#include <cstdint>

class JZFRAMEWORK_API JZColor
{
public:
    enum ColorEnum{
        Black,
        White,
        Red,
        Green,
        Blue,
        Yellow,
        Cyan,
        Magenta,
        Gray,
        LightGray,
        DarkGray,
        Orange,
        Purple,
        Transparent,
    };

    JZColor();
    JZColor(ColorEnum color);
    JZColor(uint8_t r, uint8_t g, uint8_t b,uint8_t a = 255);

    uint8_t red() const;
    uint8_t green() const;
    uint8_t blue() const;
    uint8_t alpha() const;

    void setRgb(uint8_t r, uint8_t g, uint8_t b);
    void setRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

    // 值相等: 两个 JZColor 比 rgba(JS 侧对应 equal(other))
    bool operator==(const JZColor& o) const { return m_r == o.m_r && m_g == o.m_g && m_b == o.m_b && m_a == o.m_a; }
    bool operator!=(const JZColor& o) const { return !(*this == o); }

private:
    uint8_t m_r = 0;
    uint8_t m_g = 0;
    uint8_t m_b = 0;
    uint8_t m_a = 255;
};

#endif // JZ_COLOR_H_
