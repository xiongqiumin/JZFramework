#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

// JZPrinter 冒烟测试: 用离屏 JZPainter 绘制两页 A4, 导出 PDF, 校验产物有效。
// 依赖 GDI+ 离屏绘制 (Windows), 仅 WIN32 下构建 (test/widget 已是)。

#include <cstdio>
#include <string>
#include <vector>

#include "JZFile.h"
#include "JZImage.h"
#include "JZPainter.h"
#include "JZPen.h"
#include "JZPrinter.h"

namespace
{
int g_failed = 0;
void check(bool ok, const std::string& what)
{
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what.c_str());
    if (!ok)
        ++g_failed;
}
std::string pathOf(const std::string& n) { return "jz_printer_test_" + n; }

bool readWholeFile(const std::string& p, std::vector<unsigned char>& out)
{
    JZFile f;
    if (!f.open(p, "rb"))
        return false;
    JZByteArray b = f.readAll();
    f.close();
    out.assign(b.constData(), b.constData() + b.size());
    return !out.empty();
}
} // namespace

int main()
{
    // 1) 空 printer 保存应失败
    {
        JZPrinter printer;
        check(!printer.save(pathOf("empty.pdf")), "空 printer save 失败");
    }

    // 2) 两页 A4, 用 JZPainter 绘制 (发票 / 报告风格)
    JZPrinter printer;
    for (int pg = 0; pg < 2; ++pg) {
        JZImage& page = printer.newPage(210.0, 297.0, 200);
        {
            JZPainter p(page);
            p.setPen(JZPen(JZColor(0, 0, 0, 255)));

            // 标题栏底色
            p.fillRect(printer.mm(10), printer.mm(10), printer.mm(190), printer.mm(20),
                       JZColor(220, 220, 220, 255));
            // 页面边框
            p.drawRect(printer.mm(10), printer.mm(10), printer.mm(190), printer.mm(277));
            // 分隔线
            p.drawLine(printer.mm(10), printer.mm(40), printer.mm(200), printer.mm(40));
            // 标题文字
            p.drawText(printer.mm(15), printer.mm(15),
                       pg == 0 ? "Invoice 发票" : "Report 报告");
            // 彩色块 (模拟 logo / 印章)
            p.fillRect(printer.mm(20), printer.mm(60), printer.mm(50), printer.mm(30),
                       JZColor(0, 120, 215, 255));
        }
    }

    check(printer.pageCount() == 2, "pageCount == 2");
    JZSize sz = printer.pageSize(0);
    check(sz.width() == 1654 && sz.height() == 2339, "A4@200dpi = 1654x2339");

    // 3) 保存 PDF
    const std::string pdfPath = pathOf("out.pdf");
    check(printer.save(pdfPath), "save() 成功");

    std::vector<unsigned char> bytes;
    check(readWholeFile(pdfPath, bytes), "读回 PDF 文件");
    check(bytes.size() > 1000, "PDF 非空");
    check(bytes[0] == '%' && bytes[1] == 'P' && bytes[2] == 'D' && bytes[3] == 'F',
          "PDF 以 %PDF 开头");

    bool hasEof = false;
    for (size_t i = 0; i + 5 <= bytes.size(); ++i) {
        if (std::string((const char*)&bytes[i], 5) == "%%EOF") { hasEof = true; break; }
    }
    check(hasEof, "PDF 含 %%EOF");

    std::string s((const char*)bytes.data(), bytes.size());
    check(s.find("/Type /Page") != std::string::npos, "PDF 含 /Type /Page");
    check(s.find("/DCTDecode") != std::string::npos, "PDF 含 /DCTDecode (JPEG 内嵌)");
    check(s.find("/Count 2") != std::string::npos, "PDF 含 /Count 2 (两页)");

    // 4) 单页导出图片 (调试辅助)
    check(printer.savePage(0, pathOf("page0.png")), "savePage(0) 成功");

    // 清理
    JZFile::remove(pdfPath);
    JZFile::remove(pathOf("page0.png"));
    JZFile::remove(pathOf("empty.pdf"));

    std::printf("\nresult: %s\n", g_failed == 0 ? "PASS" : "FAILED");
    std::printf("failed: %d\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
