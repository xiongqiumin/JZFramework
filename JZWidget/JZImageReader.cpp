#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "JZImageReader.h"

#include <cstring>
#include <string>

#include "JZFile.h"

// 只拿声明; stb 实现体在 JZStbImage.cpp。
#include "stb/stb_image.h"

namespace
{

void setError(std::string* error, const std::string& text)
{
    if (error)
        *error = text;
}

bool matchSig(const uint8_t* d, int len, const char* sig, int sigLen)
{
    if (!d || len < sigLen)
        return false;
    return std::memcmp(d, sig, (size_t)sigLen) == 0;
}

// 取小端 16 位 (TGA/BMP 头字段)
int le16(const uint8_t* d)
{
    return (int)d[0] | ((int)d[1] << 8);
}

// 复刻 stb__tga_test: TGA 没有魔数, 只能按 18 字节头字段的合法性猜测。
// 因此必须放在所有有魔数的格式之后探测 (stb 内部同样是最后才试 TGA)。
bool tgaTest(const uint8_t* d, int len)
{
    if (len < 18)
        return false;

    const int colorType = d[1];          // 0=真彩/灰度 1=调色板
    const int imageType = d[2];
    if (colorType > 1)
        return false;

    if (colorType == 1) {
        if (imageType != 1 && imageType != 9)   // 调色板图只可能是 1/9
            return false;
        const int mapDepth = d[7];
        if (mapDepth != 8 && mapDepth != 15 && mapDepth != 16 && mapDepth != 24 && mapDepth != 32)
            return false;
    } else {
        if (imageType != 2 && imageType != 3 && imageType != 10 && imageType != 11)
            return false;
    }

    if (le16(d + 12) < 1)   // width
        return false;
    if (le16(d + 14) < 1)   // height
        return false;

    const int bpp = d[16];
    if (colorType == 1 && bpp != 8 && bpp != 16)
        return false;
    if (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32)
        return false;
    return true;
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

// 解码失败的原因: 优先用 stb 给的, 没有则按探测出的格式兜底
std::string decodeFailureText(JZImageReader::Format fmt)
{
    const char* reason = stbi_failure_reason();
    std::string text;
    if (reason && *reason)
        text = reason;
    else
        text = "未知格式或数据损坏";

    if (fmt != JZImageReader::Format::Unknown)
        text += " (探测格式: " + std::string(JZImageReader::formatName(fmt)) + ")";
    return text;
}

} // namespace

JZImageReader::Format JZImageReader::detectFormat(const void* data, int len)
{
    const uint8_t* d = static_cast<const uint8_t*>(data);
    if (!d || len <= 0)
        return Format::Unknown;

    static const char pngSig[8] = { (char)137, 'P', 'N', 'G', 13, 10, 26, 10 };
    if (matchSig(d, len, pngSig, 8))
        return Format::Png;
    if (matchSig(d, len, "\xFF\xD8\xFF", 3))
        return Format::Jpeg;
    if (matchSig(d, len, "BM", 2))
        return Format::Bmp;
    if (matchSig(d, len, "GIF8", 4) && len >= 6 && (d[4] == '7' || d[4] == '9') && d[5] == 'a')
        return Format::Gif;
    if (matchSig(d, len, "8BPS", 4))
        return Format::Psd;
    if (matchSig(d, len, "RIFF", 4) && len >= 12 && matchSig(d + 8, len - 8, "WEBP", 4))
        return Format::WebP;

    if (tgaTest(d, len))
        return Format::Tga;

    // stb 还支持 pnm/pic/hdr 等未列入本枚举的格式, 这里一律 Unknown;
    // readData 不因探测失败而放弃, 仍会交给 stb 实际解码一次。
    return Format::Unknown;
}

const char* JZImageReader::formatName(Format fmt)
{
    switch (fmt)
    {
    case Format::Png:  return "png";
    case Format::Jpeg: return "jpeg";
    case Format::Bmp:  return "bmp";
    case Format::Gif:  return "gif";
    case Format::WebP: return "webp";
    case Format::Tga:  return "tga";
    case Format::Psd:  return "psd";
    case Format::Unknown:
    default:           return "";
    }
}

JZImageReader::Format JZImageReader::formatFromExtension(const std::string& filePath)
{
    const std::string ext = extensionOf(filePath);
    if (ext == "png")
        return Format::Png;
    if (ext == "jpg" || ext == "jpeg")
        return Format::Jpeg;
    if (ext == "bmp")
        return Format::Bmp;
    if (ext == "gif")
        return Format::Gif;
    if (ext == "webp")
        return Format::WebP;
    if (ext == "tga")
        return Format::Tga;
    if (ext == "psd")
        return Format::Psd;
    return Format::Unknown;
}

std::shared_ptr<JZImageData> JZImageReader::readData(const void* data, int len,
                                                     std::string* error)
{
    setError(error, std::string());
    if (!data || len <= 0) {
        setError(error, "图像数据为空");
        return nullptr;
    }

    const Format fmt = detectFormat(data, len);
    // 本仓库的 stb_image 未包含 webp 解码器, 直接给出明确原因
    if (fmt == Format::WebP) {
        setError(error, "暂不支持 webp 解码 (stb_image 未启用 webp)");
        return nullptr;
    }

    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(static_cast<const unsigned char*>(data),
                                              len, &w, &h, &comp, 4);   // 强制 RGBA8888
    if (!px) {
        setError(error, decodeFailureText(fmt));
        return nullptr;
    }
    if (w <= 0 || h <= 0) {
        stbi_image_free(px);
        setError(error, "图像尺寸非法");
        return nullptr;
    }

    auto img = std::make_shared<JZImageData>();
    img->setPixels(w, h, px);
    stbi_image_free(px);
    return img;
}

std::shared_ptr<JZImageData> JZImageReader::readFile(const std::string& filePath,
                                                     std::string* error)
{
    setError(error, std::string());

    if (filePath.empty()) {
        setError(error, "文件路径为空");
        return nullptr;
    }
    if (!JZFile::exists(filePath)) {
        setError(error, "文件不存在: " + filePath);
        return nullptr;
    }

    JZFile file;
    if (!file.open(filePath, "rb")) {
        setError(error, "无法打开文件: " + filePath);
        return nullptr;
    }

    const JZByteArray bytes = file.readAll();
    file.close();
    if (bytes.isEmpty()) {
        setError(error, "文件为空: " + filePath);
        return nullptr;
    }

    return readData(bytes.constData(), bytes.size(), error);
}
