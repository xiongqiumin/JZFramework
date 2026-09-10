#ifndef JZ_IMAGE_READER_H_
#define JZ_IMAGE_READER_H_

#include <cstdint>
#include <memory>
#include <string>

#include "JZImage.h"

// ==================== JZImageReader: 图像解码(读)门面 ====================
// 把压缩图像数据 (PNG/JPEG/GIF/WebP/BMP/TGA/PSD/PNM 等, stb_image 支持集) 从
// 文件或内存解成 RGBA8888 像素缓冲 (JZImageData), 供 JZImage / JZPainter 渲染
// 链路消费 —— 像素只在 JZImage(JZImageData) 里以 RGBA8888 形式流转, 由
// JZPixmap::fromImage 转成后端原生布局后才交给渲染后端贴图。
//
// 实现基于第三方单头库 stb_image (JZWidget/stb/stb_image.h; 实现宏统一在
// JZStbImage.cpp, 本文件及使用方只 include 头文件拿声明)。
// 本类只负责"读/解码"; 编码(截图保存/导出) 见 JZImageWriter, 不合并。
//
// 设计约定:
//  - 解码按需触发, 格式用文件头 (magic number) 自动探测, 调用方无需声明目标格式。
//  - 便捷路径: JZImage(filePath)/JZImage(data,len)/JZImage::decode() 已内置同一
//    解码能力 (stb), 日常使用可不必显式调用本类。
//  - 失败返回空 shared_ptr 并 (若传入 error) 填可读原因 (UTF-8)。
class JZImageReader
{
public:
    // 可解码格式 (探测与错误提示用; 子集取决于 stb_image 实际支持)
    enum class Format
    {
        Unknown,
        Png,
        Jpeg,
        Bmp,
        Gif,
        WebP,
        Tga,
        Psd
    };

    // 纯静态门面, 不实例化。
    JZImageReader() = delete;

    // 解码本地文件 -> RGBA8888 像素缓冲。文件不存在 / 格式不支持 /
    // 数据损坏均返回空。
    static std::shared_ptr<JZImageData> readFile(const std::string& filePath,
                                                 std::string* error = nullptr);

    // 解码内存数据。data 只需在调用期间保持有效, 内部会拷贝一份。
    // len <= 0 或 data 为空直接失败。
    static std::shared_ptr<JZImageData> readData(const void* data, int len,
                                                 std::string* error = nullptr);

    // 仅按文件头探测格式, 不解码像素。无法识别返回 Unknown。
    static Format detectFormat(const void* data, int len);

    // 格式的短名称 ("png"/"jpeg"/"webp"/...), Unknown 返回空串,
    // 用于错误信息与扩展名。
    static const char* formatName(Format fmt);

    // 便捷: 由扩展名推断期望格式 (用于错误提示/保存过滤器),
    // 不保证文件内容真为该格式。
    static Format formatFromExtension(const std::string& filePath);
};

#endif // !JZ_IMAGE_READER_H_
