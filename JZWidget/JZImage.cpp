#include "JZImage.h"
#include <cstring>

JZImage::JZImage(const std::string& filePath)
    : m_filePath(filePath)
{
}

JZImage::JZImage(const char* data, int len)
{}

const void* JZImage::dsc() const
{
    return m_data ? m_data->dsc() : nullptr;
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
    return 0;
}

int JZImage::height() const
{
    return 0;
}
