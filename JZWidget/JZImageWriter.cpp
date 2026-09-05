#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "JZImageWriter.h"

#include <cstring>
#include <string>

#include "JZFile.h"

// 只拿声明; stb 实现体在 JZStbImage.cpp。
#include "stb/stb_image_write.h"

namespace
{

// stbi_write_*_to_func 的写入回调: 把编码产物追加进 vector
void appendToVector(void* context, void* data, int size)
{
    std::vector<uint8_t>* out = static_cast<std::vector<uint8_t>*>(context);
    if (!out || !data || size <= 0)
        return;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    out->insert(out->end(), p, p + size);
}

int clampQuality(int quality)
{
    if (quality < 1)
        return 1;
    if (quality > 100)
        return 100;
    return quality;
}

// 由扩展名取小写后缀; 无扩展名 (或 '.' 在路径分隔符前) 返回空串
std::string extensionOf(const std::string& filePath)
{
    const size_t dot = filePath.find_last_of('.');
    const size_t sep = filePath.find_last_of("/\\");
    if (dot == std::string::npos || (sep != std::string::npos && dot < sep))
        return std::string();

    std::string ext = filePath.substr(dot + 1);
    for (size_t i = 0; i < ext.size(); ++i)
        if (ext[i] >= 'A' && ext[i] <= 'Z')
            ext[i] += 'a' - 'A';
    return ext;
}

} // namespace

const char* JZImageWriter::formatName(Format fmt)
{
    switch (fmt)
    {
    case Format::Png:  return "png";
    case Format::Bmp:  return "bmp";
    case Format::Tga:  return "tga";
    case Format::Jpeg:
    default:           return "jpeg";
    }
}

std::vector<uint8_t> JZImageWriter::writeData(const JZImage& image, Format format, int quality)
{
    std::vector<uint8_t> out;

    std::shared_ptr<JZImageData> data = image.data();
    if (!data || !data->pixels() || data->width() <= 0 || data->height() <= 0)
        return out;

    const unsigned char* px = data->pixels();
    const int w = data->width();
    const int h = data->height();
    const int stride = w * 4;   // RGBA8888 紧密排布

    bool ok = false;
    switch (format)
    {
    case Format::Png:
        ok = stbi_write_png_to_func(appendToVector, &out, w, h, 4, px, stride) != 0;
        break;
    case Format::Bmp:
        ok = stbi_write_bmp_to_func(appendToVector, &out, w, h, 4, px) != 0;
        break;
    case Format::Tga:
        ok = stbi_write_tga_to_func(appendToVector, &out, w, h, 4, px) != 0;
        break;
    case Format::Jpeg:
        ok = stbi_write_jpg_to_func(appendToVector, &out, w, h, 4, px, clampQuality(quality)) != 0;
        break;
    default:
        return out;
    }

    if (!ok)
        out.clear();   // 回调可能已部分写入, 失败时清空
    return out;
}

bool JZImageWriter::writeFile(const JZImage& image, const std::string& filePath,
                              Format format, int quality)
{
    if (filePath.empty())
        return false;

    const std::vector<uint8_t> bytes = writeData(image, format, quality);
    if (bytes.empty())
        return false;

    JZFile file;
    if (!file.open(filePath, "wb"))
        return false;

    const int size = static_cast<int>(bytes.size());
    const bool ok = file.write(reinterpret_cast<const char*>(bytes.data()), size) == size;
    file.close();
    return ok;
}

bool JZImageWriter::writeFile(const JZImage& image, const std::string& filePath, int quality)
{
    const std::string ext = extensionOf(filePath);
    Format format;
    if (ext == "png")
        format = Format::Png;
    else if (ext == "bmp")
        format = Format::Bmp;
    else if (ext == "tga")
        format = Format::Tga;
    else if (ext == "jpg" || ext == "jpeg")
        format = Format::Jpeg;
    else
        return false;   // 扩展名无法推断格式 (含无扩展名/空路径)

    return writeFile(image, filePath, format, quality);
}
