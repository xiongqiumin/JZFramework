#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

// JZImageReader / JZImageWriter 冒烟测试:
//   用 stb 直接造一张种子 PNG -> Reader 读回 -> Writer 写各格式 -> Reader 再读回校验
//   外加格式探测/扩展名推断/失败路径。
// 不依赖窗口系统, 控制台直接跑; 产物写在当前目录, 结束自行清理。

#include <cstdio>
#include <string>
#include <vector>

#include "JZFile.h"
#include "JZImage.h"
#include "JZImageReader.h"
#include "JZImageWriter.h"

// 只拿声明 (实现体在库里的 JZStbImage.cpp); 这里仅为生成种子图。
#include "stb/stb_image_write.h"

namespace
{

const int kW = 64;
const int kH = 48;
int g_failed = 0;

void check(bool ok, const std::string& what)
{
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what.c_str());
    if (!ok)
        ++g_failed;
}

std::string pathOf(const std::string& name)
{
    return "jz_img_test_" + name;
}

// 64x48 渐变图 (R 随 x, G 随 y, B 固定, A 不透明)
std::vector<unsigned char> makeGradient()
{
    std::vector<unsigned char> px((size_t)kW * kH * 4);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const size_t i = ((size_t)y * kW + x) * 4;
            px[i + 0] = (unsigned char)(x * 255 / (kW - 1));
            px[i + 1] = (unsigned char)(y * 255 / (kH - 1));
            px[i + 2] = 128;
            px[i + 3] = 255;
        }
    }
    return px;
}

bool samePixel(const std::shared_ptr<JZImageData>& d, int x, int y,
               const std::vector<unsigned char>& src)
{
    if (!d || x >= d->width() || y >= d->height())
        return false;
    const unsigned char* p = d->pixels() + ((size_t)y * d->width() + x) * 4;
    const unsigned char* s = &src[((size_t)y * kW + x) * 4];
    return p[0] == s[0] && p[1] == s[1] && p[2] == s[2] && p[3] == s[3];
}

bool pixelsMatch(const std::shared_ptr<JZImageData>& d,
                 const std::vector<unsigned char>& src)
{
    // 抽查四角 + 中心 + 两个任意点, 无损格式应逐字节一致
    const int pts[][2] = { { 0, 0 }, { kW - 1, 0 }, { 0, kH - 1 }, { kW - 1, kH - 1 },
                           { kW / 2, kH / 2 }, { 7, 3 }, { 41, 29 } };
    for (int i = 0; i < (int)(sizeof(pts) / sizeof(pts[0])); ++i)
        if (!samePixel(d, pts[i][0], pts[i][1], src))
            return false;
    return true;
}

bool readWholeFile(const std::string& path, std::vector<unsigned char>& out)
{
    JZFile f;
    if (!f.open(path, "rb"))
        return false;
    const JZByteArray bytes = f.readAll();
    f.close();
    out.assign(bytes.constData(), bytes.constData() + bytes.size());
    return !out.empty();
}

} // namespace

int main()
{
    const std::vector<unsigned char> seed = makeGradient();

    // ---- 种子图 ----
    const std::string seedPath = pathOf("seed.png");
    check(stbi_write_png(seedPath.c_str(), kW, kH, 4, seed.data(), kW * 4) != 0,
          "生成种子 " + seedPath);

    // ---- 读取 + 探测 ----
    std::string error;
    std::shared_ptr<JZImageData> src = JZImageReader::readFile(seedPath, &error);
    check(src != nullptr, "readFile(种子) 成功: " + error);
    check(src && src->width() == kW && src->height() == kH, "种子尺寸 64x48");
    check(error.empty(), "成功时 error 串为空");
    check(src && pixelsMatch(src, seed), "种子像素与原始缓冲一致");

    std::vector<unsigned char> seedBytes;
    check(readWholeFile(seedPath, seedBytes), "读入种子字节");
    check(JZImageReader::detectFormat(seedBytes.data(), (int)seedBytes.size()) ==
              JZImageReader::Format::Png,
          "detectFormat(种子) == Png");

    JZImage image(seedPath);
    check(!image.isNull(), "JZImage(种子) 非空");

    // ---- 写各格式 -> 读回 ----
    struct Case
    {
        JZImageWriter::Format fmt;
        const char* file;
        bool lossless;
    };
    const Case cases[] = {
        { JZImageWriter::Format::Png,  "out.png",  true  },
        { JZImageWriter::Format::Bmp,  "out.bmp",  true  },
        { JZImageWriter::Format::Tga,  "out.tga",  true  },
        { JZImageWriter::Format::Jpeg, "out.jpg",  false },
    };

    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); ++i) {
        const Case& c = cases[i];
        const std::string path = pathOf(c.file);

        const bool wrote = JZImageWriter::writeFile(image, path, c.fmt);
        check(wrote, std::string("writeFile ") + c.file);

        std::string err;
        std::shared_ptr<JZImageData> back = JZImageReader::readFile(path, &err);
        check(back != nullptr, std::string("readFile ") + c.file + ": " + err);
        check(back && back->width() == kW && back->height() == kH,
              std::string("回读尺寸 ") + c.file);
        if (c.lossless && back)
            check(pixelsMatch(back, seed), std::string("无损像素一致 ") + c.file);

        std::vector<unsigned char> bytes;
        check(readWholeFile(path, bytes), std::string("读回文件字节 ") + c.file);
        check(JZImageReader::detectFormat(bytes.data(), (int)bytes.size()) !=
                  JZImageReader::Format::Unknown,
              std::string("detectFormat 识别 ") + c.file);

        const std::vector<uint8_t> mem = JZImageWriter::writeData(image, c.fmt);
        check(!mem.empty(), std::string("writeData ") + c.file);
        std::shared_ptr<JZImageData> fromMem =
            JZImageReader::readData(mem.data(), (int)mem.size(), &err);
        check(fromMem && fromMem->width() == kW && fromMem->height() == kH,
              std::string("writeData 产物可被直接解码 ") + c.file);
    }

    // ---- 按扩展名推断格式 ----
    const char* extOk[] = { "ext.png", "ext.bmp", "ext.tga", "ext.jpg", "ext.jpeg" };
    for (int i = 0; i < (int)(sizeof(extOk) / sizeof(extOk[0])); ++i) {
        const std::string path = pathOf(extOk[i]);
        check(JZImageWriter::writeFile(image, path), std::string("扩展名推断写 ") + extOk[i]);
    }
    check(!JZImageWriter::writeFile(image, pathOf("ext.xyz")), "未知扩展名写失败");
    check(!JZImageWriter::writeFile(image, pathOf("ext")), "无扩展名写失败");

    // ---- 缩放 (Qt 语义: 目标尺寸由 AspectRatioMode 决定) ----
    JZImage fit = image.scaled(32, 32, JZImage::AspectRatioMode::KeepAspectRatio);
    check(!fit.isNull() && fit.width() == 32 && fit.height() == 24,
          "scaled KeepAspectRatio 64x48 -> 32x24");

    JZImage cover = image.scaled(32, 32, JZImage::AspectRatioMode::KeepAspectRatioByExpanding,
                                 JZImage::TransformationMode::SmoothTransformation);
    check(!cover.isNull() && cover.width() == 43 && cover.height() == 32,
          "scaled ByExpanding 64x48 -> 43x32");

    JZImage stretch = image.scaled(100, 20);
    check(!stretch.isNull() && stretch.width() == 100 && stretch.height() == 20,
          "scaled 忽略比例拉伸 -> 100x20");

    check(image.width() == kW && image.height() == kH, "scaled 不改动原图");

    // ---- 失败路径 ----
    std::shared_ptr<JZImageData> null0 = JZImageReader::readFile(pathOf("no_such.png"), &error);
    check(!null0 && !error.empty(), "readFile 文件不存在 -> 空 + error: " + error);

    std::shared_ptr<JZImageData> null1 = JZImageReader::readData(nullptr, 0, &error);
    check(!null1 && !error.empty(), "readData 空数据 -> 空 + error: " + error);

    const char garbage[32] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    std::shared_ptr<JZImageData> null2 = JZImageReader::readData(garbage, 32, &error);
    check(!null2 && !error.empty(), "readData 损坏数据 -> 空 + error: " + error);

    const unsigned char webpMagic[16] = { 'R','I','F','F', 0,0,0,0, 'W','E','B','P' };
    check(JZImageReader::detectFormat(webpMagic, 16) == JZImageReader::Format::WebP,
          "detectFormat 识别 webp 魔数");
    std::shared_ptr<JZImageData> null3 = JZImageReader::readData(webpMagic, 16, &error);
    check(!null3 && !error.empty(), "readData webp -> 空 + error: " + error);

    JZImage empty;
    check(empty.isNull(), "默认 JZImage 为空");
    check(empty.scaled(10, 10).isNull(), "空图 scaled -> 空图");
    check(!JZImageWriter::writeFile(empty, pathOf("empty.png"), JZImageWriter::Format::Png),
          "空图写文件失败");
    check(JZImageWriter::writeData(empty, JZImageWriter::Format::Png).empty(),
          "空图编码结果为空");
    check(JZImageReader::detectFormat(nullptr, 0) == JZImageReader::Format::Unknown,
          "detectFormat(空) == Unknown");

    // ---- 名称/扩展名映射 ----
    check(std::string(JZImageReader::formatName(JZImageReader::Format::Jpeg)) == "jpeg",
          "reader formatName(Jpeg) == jpeg");
    check(std::string(JZImageReader::formatName(JZImageReader::Format::Unknown)).empty(),
          "reader formatName(Unknown) == \"\"");
    check(JZImageReader::formatFromExtension("a/B.PNG") == JZImageReader::Format::Png,
          "formatFromExtension 忽略大小写");
    check(JZImageReader::formatFromExtension("dir.png/x") == JZImageReader::Format::Unknown,
          "formatFromExtension 忽略目录中的 '.'");
    check(JZImageReader::formatFromExtension("a.webp") == JZImageReader::Format::WebP,
          "formatFromExtension(webp)");
    check(std::string(JZImageWriter::formatName(JZImageWriter::Format::Tga)) == "tga",
          "writer formatName(Tga) == tga");

    // ---- 清理 ----
    const char* all[] = { "seed.png", "out.png", "out.bmp", "out.tga", "out.jpg",
                          "ext.png", "ext.bmp", "ext.tga", "ext.jpg", "ext.jpeg",
                          "ext.xyz", "ext", "empty.png" };
    for (int i = 0; i < (int)(sizeof(all) / sizeof(all[0])); ++i)
        JZFile::remove(pathOf(all[i]));

    std::printf("\nresult: %s\n", g_failed == 0 ? "PASS" : "FAILED");
    std::printf("failed: %d\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
