#include "JZDrawBuffer.h"
#include "JZRect.h"
#include <algorithm>

JZDrawBuffer::JZDrawBuffer(int w, int h)
{
    resize(w, h);
}

void JZDrawBuffer::resize(int w, int h)
{
    m_w = std::max(0, w);
    m_h = std::max(0, h);
    m_buf.assign((size_t)m_w * (size_t)m_h * 4, 0);
}

void JZDrawBuffer::clear(JZPixel c)
{
    clearRect(0, 0, m_w, m_h, c);
}

void JZDrawBuffer::clearRect(int x, int y, int w, int h, JZPixel c)
{
    JZRect clip = JZRect(x, y, w, h).intersect(bounds());
    if (clip.isEmpty())
        return;
    for (int yy = clip.top(); yy <= clip.bottom(); ++yy) {
        uint8_t* p = m_buf.data() + ((size_t)yy * m_w + clip.left()) * 4;
        for (int xx = clip.left(); xx <= clip.right(); ++xx) {
            p[0] = c.a; p[1] = c.r; p[2] = c.g; p[3] = c.b;
            p += 4;
        }
    }
}

void JZDrawBuffer::fillRect(int x, int y, int w, int h, JZPixel color)
{
    JZRect clip = JZRect(x, y, w, h).intersect(bounds());
    if (clip.isEmpty())
        return;

    const int sa = color.a;
    if (sa == 0)
        return;

    for (int yy = clip.top(); yy <= clip.bottom(); ++yy) {
        uint8_t* p = m_buf.data() + ((size_t)yy * m_w + clip.left()) * 4;
        for (int xx = clip.left(); xx <= clip.right(); ++xx) {
            const uint8_t da = p[0];
            // src-over (通用 ARGB)
            const int outA = sa + da - (sa * da) / 255;
            if (outA == 0) {
                p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0;
            } else {
                p[1] = (uint8_t)((color.r * sa + p[1] * (255 - sa)) / 255);
                p[2] = (uint8_t)((color.g * sa + p[2] * (255 - sa)) / 255);
                p[3] = (uint8_t)((color.b * sa + p[3] * (255 - sa)) / 255);
                p[0] = (uint8_t)outA;
            }
            p += 4;
        }
    }
}

JZPixel JZDrawBuffer::pixel(int x, int y) const
{
    if (x < 0 || y < 0 || x >= m_w || y >= m_h)
        return JZPixel{};
    const uint8_t* p = m_buf.data() + ((size_t)y * m_w + x) * 4;
    return JZPixel{p[0], p[1], p[2], p[3]};
}
