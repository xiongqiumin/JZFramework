#ifndef JZ_IMAGE_WRITER_H_
#define JZ_IMAGE_WRITER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "JZImage.h"

// ==================== JZImageWriter: 图像编码(写)门面 ====================
// 把 RGBA8888 像素缓冲 (JZImageData) 编码成压缩图像文件/内存数据
// (PNG/BMP/TGA/JPEG, stb_image_write 支持集), 用于截图保存/图片导出。
//
// 与 JZImageReader 分离: 两方向算法几乎零共享 (解码=压缩流->像素;
// 编码=像素->压缩流), 只各自薄封装 stb 对应单头库, 不应合并成一个类。
//
// 实现基于第三方单头库 stb_image_write.h (JZWidget/stb/; 实现宏统一在
// JZStbImage.cpp, 本文件及使用方只 include 头文件拿声明)。
// JPEG 编码需 quality (0~100, 默认 90); PNG/BMP/TGA 无损, 忽略 quality。
// 失败返回 false / 空 vector。
class JZImageWriter
{
public:
    // 可编码格式 (stb_image_write 支持集)
    enum class Format
    {
        Png,
        Bmp,
        Tga,
        Jpeg
    };

    // 纯静态门面, 不实例化。
    JZImageWriter() = delete;

    // 编码到文件, 格式由 format 显式指定。
    static bool writeFile(const JZImage& image, const std::string& filePath,
                          Format format, int quality = 90);

    // 编码到文件, 格式按 filePath 扩展名推断 (.png/.bmp/.tga/.jpg/.jpeg)。
    // 无法识别扩展名返回 false。
    static bool writeFile(const JZImage& image, const std::string& filePath,
                          int quality = 90);

    // 编码到内存缓冲 (供后续网络传输/再封装)。
    static std::vector<uint8_t> writeData(const JZImage& image, Format format,
                                          int quality = 90);

    // 格式短名称 ("png"/"bmp"/"tga"/"jpeg"); 供错误信息/扩展名用。
    static const char* formatName(Format fmt);
};

#endif // !JZ_IMAGE_WRITER_H_
