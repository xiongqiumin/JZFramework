#ifndef JZ_PRINTER_H_
#define JZ_PRINTER_H_
#include "JZImage.h"
#include "JZSize.h"

#include <deque>
#include <string>
#include <vector>

// ==================== JZPrinter: 把 JZPainter 绘制内容导出为多页 PDF ====================
// 复用现有离屏绘制能力: 每页是一张 JZImage (白底 RGBA8888 画布), 用户用
//   JZPainter p(page) 在其上绘制 (文字/线条/矩形/图片...), 与屏幕绘制完全一致。
// save() 把每页编码为 JPEG 内嵌进 PDF (/DCTDecode), 生成多页 PDF 文件。
//
// 坐标: 以像素为单位 (与离屏 JZImage 一致)。dpi 决定清晰度, 默认 200dpi 下
//   A4 = 1654x2339px, 文字清晰。mm() 辅助把毫米换算成像素。
//
// 说明: 本方案为"光栅化整页" (所见即所得, 文字不可选中)。适合发票/报告/单据等
//   以打印/浏览为主的场景; 若需可选中文本/矢量缩放, 后续可替换后端为 libharu 等。
//
// 用法:
//   JZPrinter printer;
//   JZImage& page = printer.newPage();        // A4 200dpi
//   { JZPainter p(page);
//     p.setFont(JZFont("Arial", 24));
//     p.drawText(printer.mm(20), printer.mm(15), "发票");
//     p.drawLine(...); p.drawImage(logo, ...); }
//   printer.save("invoice.pdf");
class JZFRAMEWORK_API JZPrinter
{
public:
    JZPrinter() = default;
    ~JZPrinter() = default;

    // 新建一页并开始绘制, 返回该页画布 (白底)。默认 A4 (210x297mm), dpi 默认 200。
    // 用返回的 JZImage 构造 JZPainter 绘制, 其作用域结束 (painter 析构) 后内容固化到该页。
    // 注意: 返回的引用仅在"下一次 newPage / save 之前"有效, 绘制应就近进行。
    JZImage& newPage(double width_mm = 210.0, double height_mm = 297.0, int dpi = 200);

    // 当前页数
    int pageCount() const;

    // 指定页像素尺寸 (dpi 下)
    JZSize pageSize(int index) const;

    // 指定页 dpi (index<0 取最后一页)
    int dpi(int index = -1) const;

    // 毫米 -> 像素 (按指定页 dpi; index<0 取最后一页)
    int mm(double m, int index = -1) const;

    // 把所有已绘制页保存为多页 PDF。成功返回 true。
    bool save(const std::string& pdfPath);

    // 调试: 把单页保存为图片 (png/jpg 按扩展名)
    bool savePage(int index, const std::string& imgPath) const;

private:
    struct Page {
        JZImage image;
        double wmm = 210.0;
        double hmm = 297.0;
        int dpi = 200;
    };

    // 自写最小 PDF writer: 把多页 JZImage 包成多页 PDF (每页一个 JPEG XObject)。
    static bool writePdf(const std::string& path, const std::deque<Page>& pages);

    std::deque<Page> m_pages;
};

#endif // JZ_PRINTER_H_
