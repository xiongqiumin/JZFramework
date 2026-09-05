#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "JZImage.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

// 只拿声明; stb 各库实现体在 JZStbImage.cpp (统一实现宏)。
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"
#include "stb/stb_image_resize2.h"

namespace
{

// 解码内存图像 -> RGBA8888 (强制 4 通道); 失败返回空
std::shared_ptr<JZImageData> decodePixelsFromMemory(const unsigned char* data, int len)
{
    if (!data || len <= 0)
        return nullptr;

    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(data, len, &w, &h, &comp, 4);
    if (!px)
        return nullptr;

    auto img = std::make_shared<JZImageData>();
    img->setPixels(w, h, px);
    stbi_image_free(px);
    return img;
}

// 解码本地文件 -> RGBA8888; 失败返回空
std::shared_ptr<JZImageData> decodePixelsFromFile(const std::string& file)
{
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load(file.c_str(), &w, &h, &comp, 4);
    if (!px)
        return nullptr;

    auto img = std::make_shared<JZImageData>();
    img->setPixels(w, h, px);
    stbi_image_free(px);
    return img;
}

// 由扩展名推断可写格式; 无法识别返回 Unknown (忽略无扩展名/跨目录的 '.' )
JZImage::Format formatFromExtension(const std::string& file)
{
    size_t dot = file.find_last_of('.');
    size_t sep = file.find_last_of("/\\");
    if (dot == std::string::npos || (sep != std::string::npos && dot < sep))
        return JZImage::Format::Unknown;

    std::string ext = file.substr(dot + 1);
    for (size_t i = 0; i < ext.size(); ++i)
        if (ext[i] >= 'A' && ext[i] <= 'Z')
            ext[i] += 'a' - 'A';

    if (ext == "png")
        return JZImage::Format::Png;
    if (ext == "bmp")
        return JZImage::Format::Bmp;
    if (ext == "tga")
        return JZImage::Format::Tga;
    if (ext == "jpg" || ext == "jpeg")
        return JZImage::Format::Jpeg;
    return JZImage::Format::Unknown;
}

// stbi_write_*_to_func 的写入回调: 把编码产物追加进 JZByteArray
void appendToByteArray(void* context, void* data, int size)
{
    JZByteArray* out = static_cast<JZByteArray*>(context);
    out->append(static_cast<const char*>(data), size);
}

// stb medium API 缩放 -> 新 JZImageData (RGBA8888); 参数非法或失败返回空。
// 边缘处理固定 Clamp (JZImage 不再对外暴露 stb 的 Filter/Edge 枚举)。
std::shared_ptr<JZImageData> stbirScale(const std::shared_ptr<JZImageData>& src,
                                        int w, int h, stbir_filter filter)
{
    if (w <= 0 || h <= 0)
        return nullptr;
    if (!src || !src->pixels() || src->width() <= 0 || src->height() <= 0)
        return nullptr;

    std::vector<uint8_t> out((size_t)w * h * 4);
    // medium API: 统一走 stbir_resize; filter 由调用方按 TransformationMode 给出,
    // 边缘恒 clamp (见 stb 头 847/851 行)。
    // 色彩空间 UINT8_SRGB, 布局 STBIR_RGBA, 直通非预乘 (与本框架像素语义一致)。
    void* ret = stbir_resize(
        src->pixels(), src->width(), src->height(), src->width() * 4,
        out.data(), w, h, w * 4,
        STBIR_RGBA, STBIR_TYPE_UINT8_SRGB,
        STBIR_EDGE_CLAMP, filter);
    if (!ret)
        return nullptr;

    auto nd = std::make_shared<JZImageData>();
    nd->setPixels(w, h, out.data());
    return nd;
}

// 依 aspect 模式计算实际缩放尺寸 (等价 Qt::scaled 的目标尺寸规则, 不填充不裁剪)
void computeScaledSize(int sw, int sh, int w, int h,
                       JZImage::AspectRatioMode mode, int& nw, int& nh)
{
    switch (mode)
    {
    case JZImage::AspectRatioMode::KeepAspectRatio: {
        const double s = std::min((double)w / sw, (double)h / sh);
        nw = std::max(1, (int)std::lround(sw * s));
        nh = std::max(1, (int)std::lround(sh * s));
        break;
    }
    case JZImage::AspectRatioMode::KeepAspectRatioByExpanding: {
        const double s = std::max((double)w / sw, (double)h / sh);
        nw = std::max(1, (int)std::lround(sw * s));
        nh = std::max(1, (int)std::lround(sh * s));
        break;
    }
    case JZImage::AspectRatioMode::IgnoreAspectRatio:
    default:
        nw = w;
        nh = h;
        break;
    }
}

} // namespace

JZImage::JZImage(const std::string& file)
{
    m_data = decodePixelsFromFile(file);
}

JZImage::JZImage(const char* data, int len)
{
    m_data = decodePixelsFromMemory(reinterpret_cast<const unsigned char*>(data), len);
}

bool JZImage::isNull() const
{
    return !m_data || m_data->width() <= 0 || m_data->height() <= 0;
}

std::shared_ptr<JZImageData> JZImage::data() const
{
    return m_data;
}

int JZImage::width() const
{
    return m_data ? m_data->width() : 0;
}

int JZImage::height() const
{
    return m_data ? m_data->height() : 0;
}

JZImage JZImage::decode(const JZByteArray& data)
{
    JZImage img;
    img.m_data = decodePixelsFromMemory(
        reinterpret_cast<const unsigned char*>(data.constData()), data.size());
    return img;
}

JZImage JZImage::fromRGBA(const uint8_t* rgba, int w, int h)
{
    JZImage img;
    if (!rgba || w <= 0 || h <= 0)
        return img;

    auto data = std::make_shared<JZImageData>();
    data->setPixels(w, h, rgba);
    img.m_data = data;
    return img;
}

JZByteArray JZImage::encode(Format format) const
{
    JZByteArray out;
    if (!m_data || !m_data->pixels() || m_data->width() <= 0 || m_data->height() <= 0)
        return out;

    const unsigned char* px = m_data->pixels();
    const int w = m_data->width();
    const int h = m_data->height();
    const int stride = w * 4;   // RGBA8888 紧密排布

    bool ok = false;
    switch (format)
    {
    case Format::Png:
        ok = stbi_write_png_to_func(appendToByteArray, &out, w, h, 4, px, stride) != 0;
        break;
    case Format::Bmp:
        ok = stbi_write_bmp_to_func(appendToByteArray, &out, w, h, 4, px) != 0;
        break;
    case Format::Tga:
        ok = stbi_write_tga_to_func(appendToByteArray, &out, w, h, 4, px) != 0;
        break;
    case Format::Jpeg:
        ok = stbi_write_jpg_to_func(appendToByteArray, &out, w, h, 4, px, 90) != 0;   // 质量固定 90
        break;
    default:
        return out;   // Unknown/Gif/WebP/Psd 不可写 -> 空
    }

    if (!ok)
        out.clear();   // 回调可能已部分写入, 失败时清空
    return out;
}

bool JZImage::save(const std::string& file) const
{
    if (!m_data || !m_data->pixels())
        return false;

    const Format fmt = formatFromExtension(file);
    if (fmt == Format::Unknown)
        return false;

    const JZByteArray bytes = encode(fmt);
    if (bytes.isEmpty())
        return false;

    std::FILE* f = std::fopen(file.c_str(), "wb");
    if (!f)
        return false;

    const bool ok =
        std::fwrite(bytes.constData(), 1, (size_t)bytes.size(), f) == (size_t)bytes.size();
    std::fclose(f);
    return ok;
}

JZImage JZImage::scaled(int w, int h, AspectRatioMode aspectRatioMode,
                        TransformationMode transformMode) const
{
    if (w <= 0 || h <= 0)
        return JZImage();
    if (!m_data || !m_data->pixels() || m_data->width() <= 0 || m_data->height() <= 0)
        return JZImage();

    // 照 Qt::TransformationMode 语义映射到 stb 滤波器:
    //   Fast   = 点采样 (最近邻), 快且锐利
    //   Smooth = 双线性 (stb Triangle 放大时等价 bilinear)
    const stbir_filter filter = (transformMode == TransformationMode::SmoothTransformation)
                                    ? STBIR_FILTER_TRIANGLE
                                    : STBIR_FILTER_POINT_SAMPLE;

    int nw = w, nh = h;
    computeScaledSize(m_data->width(), m_data->height(), w, h, aspectRatioMode, nw, nh);

    JZImage out;
    out.m_data = stbirScale(m_data, nw, nh, filter);
    return out;   // 失败时为空图 (isNull()==true)
}
