#include "JZPixmap.h"

#include "JZImage.h"

void JZPixmapData::setPixels(int w, int h, std::vector<uint8_t>&& pixels)
{
    if (w <= 0 || h <= 0 || pixels.size() != (size_t)w * h * 4) {
        m_w = 0;
        m_h = 0;
        m_ref = nullptr;
        m_pixels.clear();
        return;
    }
    m_w = w;
    m_h = h;
    m_ref = nullptr;
    m_pixels = std::move(pixels);
}

// 外部缓冲引用 (不拷贝): 只记指针 + 尺寸, 由调用方保证生命周期。
void JZPixmapData::setPixelsRef(int w, int h, const uint8_t* px)
{
    if (w <= 0 || h <= 0 || !px) {
        m_w = 0;
        m_h = 0;
        m_ref = nullptr;
        m_pixels.clear();
        return;
    }
    m_w = w;
    m_h = h;
    m_ref = px;
    m_pixels.clear();
}

bool JZPixmap::isNull() const
{
    return !m_data || m_data->isEmpty();
}

int JZPixmap::width() const
{
    return m_data ? m_data->width() : 0;
}

int JZPixmap::height() const
{
    return m_data ? m_data->height() : 0;
}

JZPixmap JZPixmap::fromData(const std::shared_ptr<JZPixmapData>& data)
{
    JZPixmap pixmap;
    if (data && !data->isEmpty())
        pixmap.m_data = data;
    return pixmap;
}

// RGBA8888 -> 32bppPARGB (B,G,R,A + 预乘)。
//   - 换序: GDI+ 的 "ARGB/RGB" 描述的是通道语义, 小端机上的内存字节序是 B,G,R,A;
//   - 预乘: PARGB 是 GDI+ 混合的原生输入 (省掉它内部的 ARGB->PARGB 转换, 同尺寸
//     实测贴图 10ms vs ARGB 28ms); 不透明像素 (截图) 就是纯拷贝, 结果与 ARGB 路径
//     逐字节一致 (含半透明/全透明像素)。
JZPixmap JZPixmap::fromImage(const JZImage& img)
{
    JZPixmap pixmap;

    const std::shared_ptr<JZImageData> data = img.data();
    if (!data || !data->pixels() || data->width() <= 0 || data->height() <= 0)
        return pixmap;

    const int w = data->width();
    const int h = data->height();
    const uint8_t* src = data->pixels();

    std::vector<uint8_t> px((size_t)w * h * 4);
    uint8_t* dst = px.data();
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = src + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x, s += 4, dst += 4) {
            const unsigned a = s[3];
            if (a == 255) {                     // 常见情形 (截图): 只换序
                dst[0] = s[2];
                dst[1] = s[1];
                dst[2] = s[0];
                dst[3] = 255;
            } else if (a == 0) {                // 全透明: PARGB 数据必须全 0
                dst[0] = dst[1] = dst[2] = dst[3] = 0;
            } else {
                dst[0] = (uint8_t)((s[2] * a) / 255);
                dst[1] = (uint8_t)((s[1] * a) / 255);
                dst[2] = (uint8_t)((s[0] * a) / 255);
                dst[3] = (uint8_t)a;
            }
        }
    }

    auto out = std::make_shared<JZPixmapData>();
    out->setPixels(w, h, std::move(px));
    pixmap.m_data = out;
    return pixmap;
}
