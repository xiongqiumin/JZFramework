// ============================================================================
// jzpdf —— JZDocument 的 PDF 后端(独立进程, 供 JZDocument::open() 通过 JZProcess 调用)
//
// 为什么是独立 exe 而不是把 PDFium 链进 JZFramework:
//   * PDFium 的 dll 有 7MB, 不该让所有用到 JZFramework 的程序都背这个体积;
//   * 进程隔离: PDF 是"不可信输入"的重灾区(PDFium 历史上漏洞不少), 解析进程崩了
//     只是这个子进程退出, 主程序不受影响, 还能加超时;
//   * 后端可替换: 换成 mutool / 自研解析器时, 只要 stdout 还是这个 JSON schema,
//     JZDocument 侧一行都不用改。
//
// 用法:
//   jzpdf <input.pdf> --image-dir <dir> [--out <file.json>] [--max-pages N] [--pretty]
// 输出: JZDocResult 同构 JSON(format/sections/blocks/images)。
//   * 图片不 base64, 而是写到 --image-dir 下, JSON 里给 path —— 避免几十 MB 的 base64;
//   * 结果 JSON 默认走 stdout, 给了 --out 就写文件。走文件是推荐用法: 结果可能几百 KB~
//     几 MB, 父进程若没有事件循环在抽管道, 子进程会被管道写阻塞(JZProcess 的 stdout
//     是靠定时器读的), 直接写文件就绕开了这个死锁。
// ============================================================================

#include "fpdfview.h"
#include "fpdf_text.h"
#include "fpdf_edit.h"
#include "fpdf_doc.h"

#include "JZCore/JZByteArray.h"
#include "JZCore/JZFile.h"
#include "JZCore/JZStd.h"
#include "JZCompress/JZCompress.h"
#include "JZJson/JZJson.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

namespace
{

// ---------------- UTF-16 → UTF-8 ----------------
std::string utf16ToUtf8(const std::vector<unsigned short> &units)
{
    std::string out;
    for (size_t i = 0; i < units.size(); i++)
    {
        unsigned int c = units[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < units.size())
        {
            unsigned int lo = units[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF)
            {
                c = 0x10000u + ((c - 0xD800u) << 10) + (lo - 0xDC00u);
                i++;
            }
            else
            {
                c = 0xFFFDu;
            }
        }
        else if (c >= 0xDC00 && c <= 0xDFFF)
        {
            c = 0xFFFDu;
        }

        if (c < 0x80)
            out += (char)c;
        else if (c < 0x800)
        {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        }
        else if (c < 0x10000)
        {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
        else
        {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// ---------------- PNG 编码(只依赖 JZCompress 的 zlib) ----------------
unsigned int crc32Buf(const unsigned char *data, size_t len)
{
    static unsigned int table[256];
    static bool inited = false;
    if (!inited)
    {
        for (unsigned int i = 0; i < 256; i++)
        {
            unsigned int c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        inited = true;
    }

    unsigned int c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void putBE32(std::string &out, unsigned int v)
{
    out += (char)((v >> 24) & 0xFF);
    out += (char)((v >> 16) & 0xFF);
    out += (char)((v >> 8) & 0xFF);
    out += (char)(v & 0xFF);
}

void pngChunk(std::string &out, const char *type, const std::string &data)
{
    putBE32(out, (unsigned int)data.size());
    out.append(type, 4);
    out += data;
    std::string crcSrc(type, 4);
    crcSrc += data;
    putBE32(out, crc32Buf((const unsigned char *)crcSrc.data(), crcSrc.size()));
}

// 输入必须是 BGRA(PDFium FPDFBitmap_BGRA), 输出 RGBA PNG
bool writePng(const std::string &path, int w, int h, const unsigned char *bgra, int stride)
{
    std::string raw;
    raw.reserve((size_t)h * (1 + (size_t)w * 4));
    for (int y = 0; y < h; y++)
    {
        const unsigned char *line = bgra + (size_t)y * (size_t)stride;
        raw += (char)0;   // filter: none
        for (int x = 0; x < w; x++)
        {
            const unsigned char *p = line + (size_t)x * 4;
            raw += (char)p[2];   // R
            raw += (char)p[1];   // G
            raw += (char)p[0];   // B
            raw += (char)p[3];   // A
        }
    }

    JZResult<JZByteArray> zlibData = jzCompressBlock(JZByteArray(raw.data(), (int)raw.size()),
                                                     JZCompressAlgorithm::Zlib, 6);
    if (zlibData.isError())
        return false;

    std::string png = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    putBE32(ihdr, (unsigned int)w);
    putBE32(ihdr, (unsigned int)h);
    ihdr += (char)8;   // bit depth
    ihdr += (char)6;   // color type: RGBA
    ihdr += (char)0;   // compression
    ihdr += (char)0;   // filter
    ihdr += (char)0;   // interlace
    pngChunk(png, "IHDR", ihdr);
    pngChunk(png, "IDAT", std::string(zlibData.value().constData(), (size_t)zlibData.value().size()));
    pngChunk(png, "IEND", std::string());

    JZFile f;
    if (!f.open(path, "wb"))
        return false;
    f.write(JZByteArray(png.data(), (int)png.size()));
    f.close();
    return true;
}

bool writeBytes(const std::string &path, const char *data, size_t size)
{
    JZFile f;
    if (!f.open(path, "wb"))
        return false;
    f.write(data, (int)size);
    f.close();
    return true;
}

std::string fileExt(const std::string &name)
{
    size_t pos = name.find_last_of('.');
    if (pos == std::string::npos)
        return std::string();
    return str_toLower(name.substr(pos + 1));
}

Json::Value bboxJson(double x, double y, double w, double h)
{
    Json::Value v(Json::arrayValue);
    v.append(x);
    v.append(y);
    v.append(w);
    v.append(h);
    return v;
}

// ---------------- 文本块切分 ----------------
// PDFium 给的是逐字符 + 每个字符的框(char box, PDF 坐标: 原点左下, y 向上)。
// 这里按"同一行的 top 接近 + 水平不回退"聚成块, 输出转成左上角原点的 bbox(与
// pdftohtml/mutool 的直觉一致: x 向右, y 向下)。
struct TextBlock
{
    std::vector<unsigned short> units;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    double top = 0;      // PDF 坐标下的行顶
    bool empty = true;
};

void appendBlock(const TextBlock &block, Json::Value &blocks)
{
    if (block.empty)
        return;
    std::string text = utf16ToUtf8(block.units);
    if (str_trimmed(text).empty())
        return;

    Json::Value b;
    b["type"] = "text";
    b["text"] = text;
    b["bbox"] = bboxJson(block.x0, block.y0, block.x1 - block.x0, block.y1 - block.y0);
    blocks.append(b);
}

// 同一行往往被 PDFium 拆成多个"片段"(CJK 尤其常见: 逐字定位 / 字框重叠 / 基线抖动),
// 直接输出会变成一堆单字块。这里做一次前向合并: 纵向中心接近 + 水平间距小 → 并成一块。
std::vector<TextBlock> mergeSameLine(const std::vector<TextBlock> &raws)
{
    std::vector<TextBlock> out;
    for (size_t i = 0; i < raws.size(); i++)
    {
        TextBlock b = raws[i];
        if (!out.empty())
        {
            TextBlock &a = out.back();
            double ah = a.y1 - a.y0;
            double bh = b.y1 - b.y0;
            // 不用 std::min/max: windows.h 的 min/max 宏会把 std::min(...) 展开坏
            double maxH = (ah > bh) ? ah : bh;
            bool degenerate = (ah <= 0.0 || bh <= 0.0);   // 有一边是零高度(空格的退化字框)
            double minH = degenerate ? maxH : ((ah < bh) ? ah : bh);
            double top = (a.y0 > b.y0) ? a.y0 : b.y0;
            double bottom = (a.y1 < b.y1) ? a.y1 : b.y1;
            double overlap = bottom - top;                 // 纵向重叠高度
            double gap = b.x0 - a.x1;                      // 水平间距
            // 同一行: 纵向重叠够多(按较矮的那个算) + 横向离得够近(按较高的那个算 ——
            // 逗号/句号这类标点的字框很矮, 拿它当基准会把阈值压得过小)。
            // 零高度块(退化字框, 常见于是空格)没法算重叠, 退化成"中心点距离"判断。
            bool sameLine = degenerate
                                ? (fabs(b.y0 - (a.y0 + a.y1) * 0.5) <= 1.5 * maxH)
                                : (overlap >= 0.4 * minH);
            if (maxH > 0 && sameLine && gap <= 0.5 * maxH)
            {
                a.units.insert(a.units.end(), b.units.begin(), b.units.end());
                if (b.x1 > a.x1) a.x1 = b.x1;
                if (b.y0 < a.y0) a.y0 = b.y0;
                if (b.y1 > a.y1) a.y1 = b.y1;
                continue;
            }
        }
        out.push_back(b);
    }
    return out;
}

void extractText(FPDF_TEXTPAGE textPage, double pageHeight, Json::Value &blocks, int debugPageNo = -1)
{
    int count = FPDFText_CountChars(textPage);
    std::vector<TextBlock> raws;
    TextBlock cur;
    double lastRight = 0;

    for (int i = 0; i < count; i++)
    {
        unsigned short buf[4] = {0, 0, 0, 0};
        if (FPDFText_GetText(textPage, i, 1, buf) <= 0)
            continue;
        unsigned int code = buf[0];

        double left = 0, right = 0, bottom = 0, top = 0;
        if (!FPDFText_GetCharBox(textPage, i, &left, &right, &bottom, &top))
            continue;

        // 换行符: 直接切块
        if (code == 0x0A || code == 0x0D)
        {
            if (!cur.empty) { raws.push_back(cur); cur = TextBlock(); }
            continue;
        }

        // 容差按字高走: 光靠字号不可靠(很多 PDF 的 FPDFText_GetFontSize 返回 <=0)
        double height = top - bottom;
        double tol = 0.5 * ((height > 0) ? height : 8.0);
        if (tol < 1.0)
            tol = 1.0;

        if (!cur.empty)
        {
            bool newLine = fabs(top - cur.top) > tol;                       // 基线明显移动 → 换行
            bool movedBack = (lastRight - left) > 0.5 * (right - left);     // 明显回退 → 新行/新列
            if (newLine || movedBack)
            {
                raws.push_back(cur);
                cur = TextBlock();
            }
        }

        if (cur.empty)
        {
            cur.x0 = left;
            cur.y0 = pageHeight - top;   // PDF 坐标 → 左上角原点
            cur.x1 = right;
            cur.y1 = pageHeight - bottom;
            cur.top = top;
        }
        else
        {
            if (left < cur.x0) cur.x0 = left;
            if (right > cur.x1) cur.x1 = right;
            if (pageHeight - top < cur.y0) cur.y0 = pageHeight - top;
            if (pageHeight - bottom > cur.y1) cur.y1 = pageHeight - bottom;
        }

        cur.units.push_back((unsigned short)code);
        cur.empty = false;
        lastRight = right;
    }
    if (!cur.empty)
        raws.push_back(cur);

    std::vector<TextBlock> merged = mergeSameLine(raws);

    // 调试: JZPDF_DEBUG_PAGE=<页码(1 基)> 时把合并前后的块打到 stderr
    const char *dbg = getenv("JZPDF_DEBUG_PAGE");
    if (dbg && atoi(dbg) == debugPageNo)
    {
        for (size_t i = 0; i < raws.size(); i++)
        {
            fprintf(stderr, "raw[%d] x=%.1f~%.1f y=%.1f~%.1f %s\n", (int)i,
                    raws[i].x0, raws[i].x1, raws[i].y0, raws[i].y1,
                    utf16ToUtf8(raws[i].units).c_str());
        }
        fprintf(stderr, "-- merged --\n");
        for (size_t i = 0; i < merged.size(); i++)
        {
            fprintf(stderr, "blk[%d] x=%.1f~%.1f y=%.1f~%.1f %s\n", (int)i,
                    merged[i].x0, merged[i].x1, merged[i].y0, merged[i].y1,
                    utf16ToUtf8(merged[i].units).c_str());
        }
    }

    for (size_t i = 0; i < merged.size(); i++)
        appendBlock(merged[i], blocks);
}

// ---------------- 图片 ----------------
// JPEG/JPEG2000 家族(DCTDecode/JPXDecode): 直接取 PDF 里的原始压缩流, 不解码,
// 还原出的就是 .jpg / .jp2 文件本身; 其它(Flate/CCITT/无压缩)走渲染位图再编 PNG。
void extractImages(FPDF_DOCUMENT doc, FPDF_PAGE page, int pageIndex, double pageHeight,
                   const std::string &imageDir, Json::Value &blocks, Json::Value &images)
{
    int objCount = FPDFPage_CountObjects(page);
    int imageIndex = 0;

    for (int i = 0; i < objCount; i++)
    {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, i);
        if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_IMAGE)
            continue;

        imageIndex++;

        // 位置: 单位方块经 matrix 变换后的外接矩形
        FS_MATRIX matrix;
        memset(&matrix, 0, sizeof(matrix));
        if (!FPDFPageObj_GetMatrix(obj, &matrix))
            continue;
        double xs[4] = {matrix.e, matrix.a + matrix.e, matrix.c + matrix.e, matrix.a + matrix.c + matrix.e};
        double ys[4] = {matrix.f, matrix.b + matrix.f, matrix.d + matrix.f, matrix.b + matrix.d + matrix.f};
        double minX = xs[0], maxX = xs[0], minY = ys[0], maxY = ys[0];
        for (int k = 1; k < 4; k++)
        {
            minX = (xs[k] < minX) ? xs[k] : minX;
            maxX = (xs[k] > maxX) ? xs[k] : maxX;
            minY = (ys[k] < minY) ? ys[k] : minY;
            maxY = (ys[k] > maxY) ? ys[k] : maxY;
        }

        // 过滤器: 判断能不能直接拿原始流
        bool jpeg = false, jp2 = false;
        int filterCount = FPDFImageObj_GetImageFilterCount(obj);
        for (int f = 0; f < filterCount; f++)
        {
            char name[64] = {0};
            if (FPDFImageObj_GetImageFilter(obj, f, name, sizeof(name)) > sizeof(name))
                continue;
            std::string filter(name);
            if (filter.find("DCT") != std::string::npos)
                jpeg = true;
            if (filter.find("JPX") != std::string::npos)
                jp2 = true;
        }

        std::string ext;
        std::string data;
        if (jpeg || jp2)
        {
            unsigned long need = FPDFImageObj_GetImageDataRaw(obj, nullptr, 0);
            if (need > 0)
            {
                data.resize(need);
                unsigned long got = FPDFImageObj_GetImageDataRaw(obj, &data[0], need);
                if (got > 0 && got <= need)
                {
                    data.resize(got);
                    ext = jp2 ? "jp2" : "jpg";
                }
                else
                {
                    data.clear();
                }
            }
        }

        if (ext.empty())
        {
            // 兜底: 渲染成 BGRA 位图再编 PNG
            FPDF_BITMAP bitmap = FPDFImageObj_GetRenderedBitmap(doc, page, obj);
            if (bitmap)
            {
                int w = FPDFBitmap_GetWidth(bitmap);
                int h = FPDFBitmap_GetHeight(bitmap);
                int stride = FPDFBitmap_GetStride(bitmap);
                const unsigned char *buffer = (const unsigned char *)FPDFBitmap_GetBuffer(bitmap);
                if (w > 0 && h > 0 && buffer)
                {
                    std::string tmp = imageDir + "/jzpdf_tmp.png";
                    if (writePng(tmp, w, h, buffer, stride))
                    {
                        JZFile f;
                        if (f.open(tmp, "rb"))
                        {
                            JZByteArray bytes = f.readAll();
                            f.close();
                            data.assign(bytes.constData(), (size_t)bytes.size());
                            ext = "png";
                            JZFile::remove(tmp);
                        }
                    }
                }
                FPDFBitmap_Destroy(bitmap);
            }
        }

        if (ext.empty())
            continue;

        std::string fileName = str_asprintf("p%d_%d.%s", pageIndex + 1, imageIndex, ext.c_str());
        std::string path = imageDir + "/" + fileName;
        if (!writeBytes(path, data.data(), data.size()))
            continue;

        int imageId = (int)images.size();
        Json::Value img;
        img["name"] = fileName;
        img["ext"] = ext;
        img["size"] = (int)data.size();
        img["path"] = path;
        images.append(img);

        Json::Value b;
        b["type"] = "image";
        b["image"] = imageId;
        b["name"] = fileName;
        b["bbox"] = bboxJson(minX, pageHeight - maxY, maxX - minX, maxY - minY);
        blocks.append(b);
    }
}

const char *lastErrorText(unsigned long code)
{
    switch (code)
    {
    case 1: return "unknown error";
    case 2: return "file not found or cannot be opened";
    case 3: return "file is not a valid PDF";
    case 4: return "password required or incorrect password";
    case 5: return "unsupported security scheme";
    case 6: return "page not found or content error";
    default: return "success";
    }
}

}  // namespace

int main(int argc, char **argv)
{
    std::string file;
    std::string imageDir;
    std::string outPath;
    int maxPages = 0;
    bool pretty = false;

    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--image-dir" && i + 1 < argc)
            imageDir = argv[++i];
        else if (a == "--out" && i + 1 < argc)
            outPath = argv[++i];
        else if (a == "--max-pages" && i + 1 < argc)
            maxPages = atoi(argv[++i]);
        else if (a == "--pretty")
            pretty = true;
        else if (a == "-h" || a == "--help")
        {
            printf("usage: jzpdf <input.pdf> --image-dir <dir> [--out <file.json>] [--max-pages N] [--pretty]\n");
            return 0;
        }
        else
            file = a;
    }

    if (file.empty())
    {
        fprintf(stderr, "jzpdf: missing input file\n");
        return 1;
    }
    if (imageDir.empty())
    {
        fprintf(stderr, "jzpdf: --image-dir is required (images are written to disk, not base64)\n");
        return 1;
    }

    FPDF_InitLibrary();

    FPDF_DOCUMENT doc = FPDF_LoadDocument(file.c_str(), nullptr);
    if (!doc)
    {
        fprintf(stderr, "jzpdf: load failed: %s\n", lastErrorText(FPDF_GetLastError()));
        FPDF_DestroyLibrary();
        return 2;
    }

    Json::Value root;
    root["format"] = "pdf";

    // 标题: 元数据 Title, 没有就留空(JZDocument 侧会退回首个文本块)
    unsigned long titleLen = FPDF_GetMetaText(doc, "Title", nullptr, 0);
    if (titleLen > 2)
    {
        std::vector<char> buf(titleLen + 2, 0);
        if (FPDF_GetMetaText(doc, "Title", &buf[0], (unsigned long)buf.size()) > 0)
        {
            std::string title(&buf[0]);
            title = str_trimmed(title);
            if (!title.empty())
                root["title"] = title;
        }
    }

    int pageCount = FPDF_GetPageCount(doc);
    if (maxPages > 0 && maxPages < pageCount)
        pageCount = maxPages;

    Json::Value sections(Json::arrayValue);
    Json::Value images(Json::arrayValue);

    for (int p = 0; p < pageCount; p++)
    {
        FPDF_PAGE page = FPDF_LoadPage(doc, p);
        if (!page)
            continue;

        double pageWidth = FPDF_GetPageWidthF(page);
        double pageHeight = FPDF_GetPageHeightF(page);

        Json::Value blocks(Json::arrayValue);

        // 先文本, 再图片: 与阅读顺序大体一致(严格按 y 排序需要页面对象遍历, 见 TODO)
        FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
        if (textPage)
        {
            extractText(textPage, pageHeight, blocks, p + 1);
            FPDFText_ClosePage(textPage);
        }
        extractImages(doc, page, p, pageHeight, imageDir, blocks, images);

        Json::Value sec;
        sec["name"] = str_asprintf("page %d", p + 1);
        sec["width"] = pageWidth;
        sec["height"] = pageHeight;
        sec["blocks"] = blocks;
        sections.append(sec);

        FPDF_ClosePage(page);
    }

    root["pages"] = pageCount;
    root["sections"] = sections;
    root["images"] = images;

    FPDF_CloseDocument(doc);
    FPDF_DestroyLibrary();

    // emitUTF8: 中文不要转义成 \uXXXX(体积更小, 人眼也能读)
    Json::StreamWriterBuilder writer;
    writer["emitUTF8"] = true;
    writer["indentation"] = pretty ? "  " : "";
    std::string out = Json::writeString(writer, root);

    if (!outPath.empty())
    {
        JZFile f;
        if (!f.open(outPath, "wb"))
        {
            fprintf(stderr, "jzpdf: cannot write %s\n", outPath.c_str());
            return 3;
        }
        f.write(JZByteArray(out.data(), (int)out.size()));
        f.close();
        return 0;
    }

    fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}
