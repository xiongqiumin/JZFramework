#ifndef JZ_BRUSH_H_
#define JZ_BRUSH_H_

#include "JZColor.h"
#include <cstdint>

class JZBrush
{
public:
    static constexpr uint8_t kOpaCover = 255;

    JZBrush() = default;
    JZBrush(const JZColor& color, uint8_t opa = kOpaCover);

    void setColor(const JZColor& color);
    JZColor color() const;

    void setOpacity(uint8_t opa);
    uint8_t opacity() const;

private:
    JZColor m_color = JZColor::Black();
    uint8_t m_opa = kOpaCover;
};

#endif // JZ_BRUSH_H_
