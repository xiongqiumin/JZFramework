#include "JZPrinter.h"
#include "JZImageWriter.h"
#include "JZFile.h"

#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

JZImage& JZPrinter::newPage(double width_mm, double height_mm, int dpi)
{
    if (dpi < 1)
        dpi = 200;
    int wpx = (int)std::lround(width_mm / 25.4 * dpi);
    int hpx = (int)std::lround(height_mm / 25.4 * dpi);
    if (wpx < 1) wpx = 1;
    if (hpx < 1) hpx = 1;

    Page pg;
    pg.image = JZImage::create(wpx, hpx, JZColor(255, 255, 255, 255));
    pg.wmm = width_mm;
    pg.hmm = height_mm;
    pg.dpi = dpi;
    m_pages.push_back(std::move(pg));
    return m_pages.back().image;
}

int JZPrinter::pageCount() const
{
    return (int)m_pages.size();
}

JZSize JZPrinter::pageSize(int index) const
{
    if (index < 0)
        index = (int)m_pages.size() - 1;
    if (index < 0 || index >= (int)m_pages.size())
        return JZSize();
    return JZSize(m_pages[index].image.width(), m_pages[index].image.height());
}

int JZPrinter::dpi(int index) const
{
    if (index < 0)
        index = (int)m_pages.size() - 1;
    if (index < 0 || index >= (int)m_pages.size())
        return 0;
    return m_pages[index].dpi;
}

int JZPrinter::mm(double m, int index) const
{
    int d = dpi(index);
    if (d <= 0)
        d = 200;
    return (int)std::lround(m / 25.4 * d);
}

bool JZPrinter::savePage(int index, const std::string& imgPath) const
{
    if (index < 0 || index >= (int)m_pages.size())
        return false;
    return m_pages[index].image.save(imgPath);
}

bool JZPrinter::save(const std::string& pdfPath)
{
    if (m_pages.empty())
        return false;
    return writePdf(pdfPath, m_pages);
}

// ==================== 最小 PDF writer (零依赖) ====================
// 生成 PDF 1.4 多页文档: 每页一个 JPEG 图像 XObject (/DCTDecode),
// 内容流用 cm 把图像拉伸到整页 MediaBox。图像字节直接来自 JZImageWriter。

namespace {

std::string dtos(double v)
{
    char tmp[32];
    std::snprintf(tmp, sizeof(tmp), "%.2f", v);
    return std::string(tmp);
}

} // namespace

bool JZPrinter::writePdf(const std::string& path, const std::deque<Page>& pages)
{
    std::vector<uint8_t> buf;
    auto append = [&](const std::string& s) { buf.insert(buf.end(), s.begin(), s.end()); };
    auto appendBytes = [&](const uint8_t* d, size_t n) { buf.insert(buf.end(), d, d + n); };

    append("%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");

    // 对象偏移表 (1-based): 1=Catalog, 2=Pages, 页 k -> page=3+3k, content=4+3k, image=5+3k
    std::vector<size_t> objOff;
    auto objStart = [&](int num) {
        if ((int)objOff.size() <= num)
            objOff.resize(num + 1, 0);
        objOff[num] = buf.size();
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "%d 0 obj\n", num);
        append(tmp);
    };
    auto objEnd = [&]() { append("endobj\n"); };

    int n = (int)pages.size();
    int totalObj = 2 + 3 * n;

    // 1 Catalog
    objStart(1);
    append("<< /Type /Catalog /Pages 2 0 R >>\n");
    objEnd();

    // 2 Pages
    {
        std::string kids = "<< /Type /Pages /Count " + std::to_string(n) + " /Kids [";
        for (int k = 0; k < n; ++k)
            kids += std::to_string(3 + 3 * k) + " 0 R ";
        kids += "] >>\n";
        objStart(2);
        append(kids);
        objEnd();
    }

    for (int k = 0; k < n; ++k) {
        const Page& pg = pages[k];
        int pageObj = 3 + 3 * k, contentObj = 4 + 3 * k, imageObj = 5 + 3 * k;
        double wpt = pg.wmm / 25.4 * 72.0;
        double hpt = pg.hmm / 25.4 * 72.0;

        std::vector<uint8_t> jpg = JZImageWriter::writeData(pg.image, JZImageWriter::Format::Jpeg, 90);
        if (jpg.empty())
            return false;

        int wpx = pg.image.width();
        int hpx = pg.image.height();

        // Page
        objStart(pageObj);
        std::string po = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 "
            + dtos(wpt) + " " + dtos(hpt) + "] /Resources << /XObject << /Im"
            + std::to_string(k) + " " + std::to_string(imageObj) + " 0 R >> >> /Contents "
            + std::to_string(contentObj) + " 0 R >>\n";
        append(po);
        objEnd();

        // Content stream: 把图像拉伸到整页
        std::string content = "q\n" + dtos(wpt) + " 0 0 " + dtos(hpt)
            + " 0 0 cm\n/Im" + std::to_string(k) + " Do\nQ\n";
        objStart(contentObj);
        append("<< /Length " + std::to_string(content.size()) + " >>\nstream\n");
        append(content);
        append("endstream\n");
        objEnd();

        // Image XObject (DCTDecode = 标准 JPEG 流)
        objStart(imageObj);
        std::string io = "<< /Type /XObject /Subtype /Image /Width " + std::to_string(wpx)
            + " /Height " + std::to_string(hpx)
            + " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length "
            + std::to_string(jpg.size()) + " >>\nstream\n";
        append(io);
        appendBytes(jpg.data(), jpg.size());
        append("\nendstream\n");
        objEnd();
    }

    // xref
    size_t xrefOffset = buf.size();
    {
        std::string xh = "xref\n0 " + std::to_string(totalObj + 1) + "\n";
        append(xh);
        append("0000000000 65535 f \n");
        for (int i = 1; i <= totalObj; ++i) {
            char tmp[32];
            std::snprintf(tmp, sizeof(tmp), "%010zu 00000 n \n", objOff[i]);
            append(tmp);
        }
        std::string tr = "trailer\n<< /Size " + std::to_string(totalObj + 1)
            + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";
        append(tr);
    }

    JZFile file;
    if (!file.open(path, "wb"))
        return false;
    bool ok = file.write(reinterpret_cast<const char*>(buf.data()), (int)buf.size())
              == (int)buf.size();
    file.close();
    return ok;
}
