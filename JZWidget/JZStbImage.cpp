// ==================== stb 系列单头库: 统一实现编译单元 ====================
// 所有 stb 库的 IMPLEMENTATION 只在此文件提供一次, 其余 TU (JZImage.cpp、
// 未来的 JZImageReader/Writer 实现等) 只 #include 头文件拿声明,
// 避免宏重复定义与多份实现体。
//
// 头文件来源: E:\libs\stb (完整仓库), 仅拷贝用到的 3 个头 + LICENSE
// 到 JZWidget/stb/。各自许可见 stb/LICENSE。
//   - stb_image.h          图像解码 (JZImageReader)
//   - stb_image_write.h    图像编码 PNG/BMP/TGA/JPEG (JZImageWriter)
//   - stb_image_resize2.h  RGBA8888 像素平滑缩放 (JZImage::resize)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#pragma warning(push)
// 第三方 C 库常见告警, 不在本项目代码中, 统一静默 (本项目告警级别较严)。
#pragma warning(disable : 4100 4127 4189 4244 4267 4702 4996 6011 6387)

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize2.h"

#pragma warning(pop)
