#ifndef JZ_COLOR_H_
#define JZ_COLOR_H_

#include <cstdint>

class JZColor
{
public:
    JZColor();
    JZColor(uint8_t r, uint8_t g, uint8_t b);

    uint8_t red() const;
    uint8_t green() const;
    uint8_t blue() const;

    void setRgb(uint8_t r, uint8_t g, uint8_t b);

    // LVGL 互转（内部使用，声明在 .cpp）
    struct lv_color_pod { uint8_t blue, green, red, alpha; };

    lv_color_pod toLvColor() const;
    static JZColor fromLvColor(lv_color_pod c);

    // 预定义颜色
    static JZColor Black();
    static JZColor White();
    static JZColor Red();
    static JZColor Green();
    static JZColor Blue();

private:
    uint8_t m_r = 0;
    uint8_t m_g = 0;
    uint8_t m_b = 0;
};

#endif // JZ_COLOR_H_
