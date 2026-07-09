#include "JZImage.h"
#include "lvgl.h"
#include <cstring>

JZImage::JZImage(const std::string& filePath)
    : m_filePath(filePath)
{
}

JZImage::JZImage(const char* data, int len)
{
    auto* src = (const lv_image_dsc_t*)data;
    m_dsc.reset(new lv_image_dsc_t(*src));
}

const lv_image_dsc_t* JZImage::dsc() const
{
    return m_dsc.get();
}

bool JZImage::isFile() const
{
    return !m_filePath.empty();
}

std::string JZImage::filePath() const
{
    return m_filePath;
}

int JZImage::width() const
{
    if (m_dsc) return m_dsc->header.w;
    return 0;
}

int JZImage::height() const
{
    if (m_dsc) return m_dsc->header.h;
    return 0;
}
