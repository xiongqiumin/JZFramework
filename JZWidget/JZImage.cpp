#include "JZImage.h"
#include <cstring>

JZImage::JZImage(const std::string& filePath)
    : m_filePath(filePath)
{
}

JZImage::JZImage(const char* data, int len)
{
    // TODO: 图像解码 (PNG/JPEG) 后填充 JZImageData
    (void)data;
    (void)len;
}

bool JZImage::isNull() const
{
    return !m_data || m_data->width() <= 0 || m_data->height() <= 0;
}

std::shared_ptr<JZImageData> JZImage::data() const
{
    return m_data;
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
    return m_data ? m_data->width() : 0;
}

int JZImage::height() const
{
    return m_data ? m_data->height() : 0;
}
