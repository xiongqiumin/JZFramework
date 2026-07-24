#ifndef JZ_DRAW_BUFFER_H_
#define JZ_DRAW_BUFFER_H_

#include <cstdint>
#include <vector>
#include "JZRect.h"

// ARGB8888 像素 (LVGL 用 lv_color_t, 这里用 JZFramework 自有格式, 带 alpha 以支持混合)
struct JZPixel {
    uint8_t a = 255;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

// 显示/图层像素缓冲区 (等价于 LVGL 的 draw_buf / layer->buf)。
// 这里只做软件光栅化的最底层: 矩形 + alpha 混合, 算法参考 lv_draw_sw。
class JZDrawBuffer
{
public:
    JZDrawBuffer(int w = 0, int h = 0);

    void resize(int w, int h);

    int width() const  { return m_w; }
    int height() const { return m_h; }
    int stride() const { return m_w; }   // ARGB8888, 4 字节/像素

    uint8_t* data()             { return m_buf.data(); }
    const uint8_t* data() const { return m_buf.data(); }

    // 整屏/整层清成单色 (刷新脏区前用它铺底)
    void clear(JZPixel c);
    void clearRect(int x, int y, int w, int h, JZPixel c);

    // alpha 混合填充 (src-over), 自动裁剪到缓冲区范围内
    void fillRect(int x, int y, int w, int h, JZPixel color);

    JZPixel pixel(int x, int y) const;
    JZRect  bounds() const { return JZRect(0, 0, m_w, m_h); }

private:
    int m_w = 0, m_h = 0;
    std::vector<uint8_t> m_buf; // ARGB8888
};

#endif // JZ_DRAW_BUFFER_H_
