#include "JZChart.h"
#include "JZPainter.h"
#include "JZPainterPath.h"
#include "JZFont.h"
#include "JZFontMetrics.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

JZ_OBJECT_IMPL(JZChart, JZWidget)

namespace {
const double kPi = 3.14159265358979323846;

// 图例列宽 (色块 + 间距 + 最长文字 + 内边距)
int legendWidthOf(const std::vector<std::string>& names, const JZFont& font)
{
    int maxW = 0;
    for (const auto& n : names)
        maxW = std::max(maxW, JZFontMetrics::textWidth(n.empty() ? " " : n, font));
    return 14 + 6 + maxW + 16;
}
} // namespace

JZChart::JZChart(JZWidget* parent)
    : JZWidget(parent)
{
}

JZChart::~JZChart()
{
}

JZColor JZChart::paletteColor(int i)
{
    static const JZColor palette[] = {
        JZColor(JZColor::Blue), JZColor(JZColor::Red), JZColor(0x33, 0x99, 0x33),
        JZColor(JZColor::Orange), JZColor(JZColor::Purple), JZColor(JZColor::Cyan),
        JZColor(0x8c, 0x56, 0x4b), JZColor(0xe9, 0x1d, 0x63),
    };
    const int n = (int)(sizeof(palette) / sizeof(palette[0]));
    if (i < 0) i = 0;
    return palette[i % n];
}

JZSize JZChart::sizeHint() const
{
    int w = m_width > 0 ? m_width : 240;
    return JZSize(w, 160);
}

void JZChart::setChartType(ChartType type)
{
    m_type = type;
}

JZChart::ChartType JZChart::chartType() const
{
    return m_type;
}

int JZChart::addSeries(const std::string& name, const JZColor& color)
{
    Series s;
    s.name = name;
    s.color = color;
    m_series.push_back(std::move(s));
    return (int)m_series.size() - 1;
}

void JZChart::setSeriesData(int index, const std::vector<double>& data)
{
    if (index >= 0 && index < (int)m_series.size())
        m_series[index].data = data;
}

void JZChart::setSeriesName(int index, const std::string& name)
{
    if (index >= 0 && index < (int)m_series.size())
        m_series[index].name = name;
}

void JZChart::setSeriesColor(int index, const JZColor& color)
{
    if (index >= 0 && index < (int)m_series.size())
        m_series[index].color = color;
}

void JZChart::clearSeries()
{
    m_series.clear();
}

int JZChart::seriesCount() const
{
    return (int)m_series.size();
}

const JZChart::Series& JZChart::series(int index) const
{
    return m_series[index];
}

JZChart::Series& JZChart::series(int index)
{
    return m_series[index];
}

// ---- 数值 X 轴 ----

void JZChart::setSeriesKeys(int index, const std::vector<double>& keys)
{
    if (index >= 0 && index < (int)m_series.size())
        m_series[index].keys = keys;
}

void JZChart::setSeriesDataXY(int index, const std::vector<double>& keys, const std::vector<double>& values)
{
    if (index < 0 || index >= (int)m_series.size())
        return;
    Series& s = m_series[index];
    s.data = values;
    // X/Y 必须一一对应: 长度不一致时按短的截断(宁缺勿错位 —— 错位画出来的曲线是假的)
    size_t n = std::min(keys.size(), values.size());
    s.keys.assign(keys.begin(), keys.begin() + n);
    s.data.resize(n);
}

const std::vector<double>& JZChart::seriesKeys(int index) const
{
    static const std::vector<double> empty;
    if (index < 0 || index >= (int)m_series.size())
        return empty;
    return m_series[index].keys;
}

bool JZChart::hasSeriesKeys(int index) const
{
    if (index < 0 || index >= (int)m_series.size())
        return false;
    const Series& s = m_series[index];
    return !s.keys.empty() && s.keys.size() == s.data.size();
}

void JZChart::setSeriesValueAxis(int index, int axis)
{
    if (index >= 0 && index < (int)m_series.size())
        m_series[index].valueAxis = (axis == 1) ? 1 : 0;
}

int JZChart::seriesValueAxis(int index) const
{
    if (index < 0 || index >= (int)m_series.size())
        return 0;
    return m_series[index].valueAxis;
}

bool JZChart::hasValueAxis(int axis) const
{
    for (const auto& s : m_series)
        if (s.valueAxis == axis && !s.data.empty())
            return true;
    return false;
}

void JZChart::setAxisLabels(const std::string& xLabel, const std::string& yLabel,
                            const std::string& y2Label)
{
    m_xLabel = xLabel;
    m_yLabel = yLabel;
    m_y2Label = y2Label;
}

void JZChart::setData(const std::vector<std::vector<double>>& seriesList)
{
    m_series.clear();
    int n = (int)seriesList.size();
    for (int i = 0; i < n; i++) {
        Series s;
        char buf[16];
        snprintf(buf, sizeof(buf), "Series %d", i + 1);
        s.name = buf;
        s.color = paletteColor(i);
        s.data = seriesList[i];
        m_series.push_back(std::move(s));
    }
}

void JZChart::setCategories(const std::vector<std::string>& categories)
{
    m_categories = categories;
}

const std::vector<std::string>& JZChart::categories() const
{
    return m_categories;
}

void JZChart::setTitle(const std::string& title)
{
    m_title = title;
}

const std::string& JZChart::title() const
{
    return m_title;
}

void JZChart::setShowGrid(bool on)   { m_showGrid = on; }
void JZChart::setShowLegend(bool on) { m_showLegend = on; }
void JZChart::setShowAxis(bool on)   { m_showAxis = on; }
void JZChart::setShowPoint(bool on)  { m_showPoint = on; }
void JZChart::setShowValue(bool on)  { m_showValue = on; }

void JZChart::computeRange(double& yMin, double& yMax) const
{
    computeRangeFor(0, yMin, yMax);
}

void JZChart::computeRangeFor(int axis, double& yMin, double& yMax) const
{
    bool any = false;
    double lo = 0, hi = 0;
    for (const auto& s : m_series) {
        if (s.valueAxis != axis)
            continue;
        for (double v : s.data) {
            if (!any) { lo = hi = v; any = true; }
            else { lo = std::min(lo, v); hi = std::max(hi, v); }
        }
    }
    if (!any) { yMin = 0; yMax = 1; return; }
    // 柱状图以 0 为基线, 范围必须包含 0
    if (m_type == BarChart) {
        lo = std::min(lo, 0.0);
        hi = std::max(hi, 0.0);
    }
    if (lo == hi) { lo -= 1; hi += 1; }
    else { double pad = (hi - lo) * 0.1; lo -= pad; hi += pad; }
    yMin = lo;
    yMax = hi;
}

// X 值域 + 模式判定:
//   数值模式 —— 有任一条序列给了 keys(且与 data 等长): 取所有 keys 的并集;
//   索引模式 —— 否则 X = 0..maxN-1(原来的行为, 分类标签也按索引对齐)。
// 同一张图里"给了 keys"和"没给 keys"的序列不要混用(number 模式下没给 keys 的那条按索引画,
// 只会画到左上角一小段 —— 属于调用方的错误用法)。
void JZChart::computeXRange(double& xMin, double& xMax, bool& numeric) const
{
    numeric = false;
    xMin = 0; xMax = 1;
    int maxN = 0;
    bool any = false;
    for (int si = 0; si < (int)m_series.size(); si++) {
        const Series& s = m_series[si];
        maxN = std::max(maxN, (int)s.data.size());
        if (!hasSeriesKeys(si))
            continue;
        numeric = true;
        for (double k : s.keys) {
            if (!any) { xMin = xMax = k; any = true; }
            else { xMin = std::min(xMin, k); xMax = std::max(xMax, k); }
        }
    }
    if (!numeric) {
        xMin = 0;
        xMax = (maxN > 1) ? (double)(maxN - 1) : 1.0;
        return;
    }
    if (!any) { xMin = 0; xMax = 1; return; }
    if (xMin == xMax) { xMin -= 0.5; xMax += 0.5; }
}

void JZChart::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width(), h = height();
    if (w <= 0 || h <= 0)
        return;

    // 背景
    p.setBrush(JZBrush(JZColor(0xff, 0xff, 0xff)));
    p.setPen(JZPen(JZColor(0xee, 0xee, 0xee), 1));
    p.drawRect(0, 0, w, h);

    // 标题
    int titleH = 8;
    if (!m_title.empty()) {
        JZFont tf(13);
        tf.setBold(true);
        p.setPen(JZPen(JZColor(0x22, 0x22, 0x22), 1));
        p.setFont(tf);
        p.drawText(10, 6, m_title.c_str());
        titleH = 24;
    }

    JZRect content(0, titleH, w, h - titleH);
    if (content.width() < 20 || content.height() < 20)
        return;

    if (m_type == PieChart) {
        drawPieChart(p, content);
        return;
    }

    // 图例列 (右侧)
    std::vector<std::string> names;
    std::vector<JZColor> colors;
    for (const auto& s : m_series) {
        names.push_back(s.name);
        colors.push_back(s.color);
    }
    int legendW = 0;
    if (m_showLegend && !names.empty())
        legendW = legendWidthOf(names, JZFont(11));

    // 右 Y 轴(有条序列用 axis=1 才画)与两条轴标题都要占版面
    const bool rightAxis = m_showAxis && hasValueAxis(1);

    int left = m_showAxis ? 46 : 10;
    int right = 10 + legendW + (rightAxis ? 34 : 0);   // 右轴刻度的位置
    int bottom = m_showAxis ? 24 : 10;
    if (m_showAxis && !m_xLabel.empty())
        bottom += 16;                                   // X 轴标题
    int top = content.y() + 6;
    if (m_showAxis && (!m_yLabel.empty() || (rightAxis && !m_y2Label.empty())))
        top += 16;                                      // 左右 Y 轴标题
    JZRect plot(left, top, std::max(1, w - left - right),
                std::max(1, content.height() - (top - content.y()) - bottom));

    double yMin, yMax, y2Min = 0, y2Max = 1;
    computeRangeFor(0, yMin, yMax);
    if (rightAxis)
        computeRangeFor(1, y2Min, y2Max);

    double xMin, xMax;
    bool numericX;
    computeXRange(xMin, xMax, numericX);

    if (m_showAxis || m_showGrid)
        drawAxis(p, plot, yMin, yMax, y2Min, y2Max, rightAxis, xMin, xMax, numericX);

    if (m_type == BarChart)
        drawBarChart(p, plot, yMin, yMax);
    else
        drawLineChart(p, plot, yMin, yMax, y2Min, y2Max, xMin, xMax, numericX);

    if (legendW > 0) {
        JZRect la(w - legendW, content.y() + 6, legendW, content.height() - 12);
        drawLegend(p, la, names, colors);
    }
}

void JZChart::drawAxis(JZPainter& p, const JZRect& plot,
                       double yMin, double yMax, double y2Min, double y2Max, bool rightAxis,
                       double xMin, double xMax, bool numericX) const
{
    JZFont axisFont(10);
    const int divs = 4;

    if (m_showGrid) {
        p.setPen(JZPen(JZColor(0xee, 0xee, 0xee), 1));
        for (int i = 0; i <= divs; i++) {
            double v = yMin + (yMax - yMin) * i / divs;
            int y = yAt(v, yMin, yMax, plot);
            p.drawLine(plot.left(), y, plot.right(), y);
        }
    }

    if (m_showAxis) {
        // 轴线 (有右轴时右边界也画一条)
        p.setPen(JZPen(JZColor(0x88, 0x88, 0x88), 1));
        p.drawLine(plot.left(), plot.top(), plot.left(), plot.bottom());
        p.drawLine(plot.left(), plot.bottom(), plot.right(), plot.bottom());
        if (rightAxis)
            p.drawLine(plot.right(), plot.top(), plot.right(), plot.bottom());

        // 左 Y 轴刻度标签
        p.setPen(JZPen(JZColor(0x55, 0x55, 0x55), 1));
        p.setFont(axisFont);
        for (int i = 0; i <= divs; i++) {
            double v = yMin + (yMax - yMin) * i / divs;
            int y = yAt(v, yMin, yMax, plot);
            std::string label = fmtValue(v);
            int tw = JZFontMetrics::textWidth(label, axisFont);
            p.drawText(plot.left() - 4 - tw, y - axisFont.size() / 2, label.c_str());
        }

        // 右 Y 轴刻度标签 (只在真的用了右轴时画)
        if (rightAxis) {
            for (int i = 0; i <= divs; i++) {
                double v = y2Min + (y2Max - y2Min) * i / divs;
                int y = yAt(v, y2Min, y2Max, plot);
                std::string label = fmtValue(v);
                p.drawText(plot.right() + 4, y - axisFont.size() / 2, label.c_str());
            }
        }

        // X 轴刻度标签: 数值模式按值域等分(等价 qcustomplot 的数值 tick);
        // 索引模式沿用原行为(0 / 中点 / 末点, 有分类标签优先用分类)
        if (numericX) {
            for (int i = 0; i <= divs; i++) {
                double v = xMin + (xMax - xMin) * i / divs;
                int x = xAtValue(v, xMin, xMax, plot);
                std::string label = fmtValue(v);
                int tw = JZFontMetrics::textWidth(label, axisFont);
                p.drawText(x - tw / 2, plot.bottom() + 4, label.c_str());
            }
        } else {
            int maxN = 0;
            for (const auto& s : m_series)
                maxN = std::max(maxN, (int)s.data.size());
            if (maxN > 0) {
                int ticks[] = { 0, maxN / 2, maxN - 1 };
                for (int t : ticks) {
                    if (t < 0 || t >= maxN) continue;
                    int x = xAt(t, maxN, plot);
                    std::string label;
                    if (t < (int)m_categories.size())
                        label = m_categories[t];
                    else {
                        char buf[16];
                        snprintf(buf, sizeof(buf), "%d", t);
                        label = buf;
                    }
                    int tw = JZFontMetrics::textWidth(label, axisFont);
                    p.drawText(x - tw / 2, plot.bottom() + 4, label.c_str());
                }
            }
        }

        // 轴标题 (等价 qcustomplot 的 axis->setLabel): X 在刻度行下面居中, Y 在轴上方
        JZFont labelFont(11);
        p.setFont(labelFont);
        p.setPen(JZPen(JZColor(0x33, 0x33, 0x33), 1));
        if (!m_xLabel.empty()) {
            int tw = JZFontMetrics::textWidth(m_xLabel, labelFont);
            p.drawText(plot.left() + (plot.width() - tw) / 2, plot.bottom() + 4 + axisFont.size() + 3,
                       m_xLabel.c_str());
        }
        if (!m_yLabel.empty())
            p.drawText(plot.left(), plot.top() - 4 - labelFont.size(), m_yLabel.c_str());
        if (rightAxis && !m_y2Label.empty()) {
            int tw = JZFontMetrics::textWidth(m_y2Label, labelFont);
            p.drawText(plot.right() - tw, plot.top() - 4 - labelFont.size(), m_y2Label.c_str());
        }
    }
}

void JZChart::drawLineChart(JZPainter& p, const JZRect& plot,
                            double yMin, double yMax, double y2Min, double y2Max,
                            double xMin, double xMax, bool numericX) const
{
    for (int si = 0; si < (int)m_series.size(); si++) {
        const Series& s = m_series[si];
        int n = (int)s.data.size();
        if (n == 0)
            continue;

        // 本序列用哪条 Y 轴 / 哪种 X 映射 (数值模式见 computeXRange 的说明)
        const double aMin = (s.valueAxis == 1) ? y2Min : yMin;
        const double aMax = (s.valueAxis == 1) ? y2Max : yMax;
        const bool keys = hasSeriesKeys(si);
        const int ni = keys ? (int)s.keys.size() : n;

        auto xOf = [&](int i) {
            return keys ? xAtValue(s.keys[i], xMin, xMax, plot) : xAt(i, n, plot);
        };

        p.setPen(JZPen(s.color, 2));
        if (ni >= 2) {
            int px = xOf(0), py = yAt(s.data[0], aMin, aMax, plot);
            for (int i = 1; i < ni; i++) {
                int cx = xOf(i), cy = yAt(s.data[i], aMin, aMax, plot);
                p.drawLine(px, py, cx, cy);
                px = cx; py = cy;
            }
        }

        // 数据点: 点太密就不画(几千个点的曲线画点只会糊成一片, 与 qcustomplot 的
        // "setScatterStyle 关掉"同理)。阈值 200 与 plot 宽度无关, 只看数量。
        if (m_showPoint && ni <= 200) {
            p.setBrush(JZBrush(s.color));
            for (int i = 0; i < ni; i++) {
                int cx = xOf(i), cy = yAt(s.data[i], aMin, aMax, plot);
                p.fillEllipse(cx - 2, cy - 2, 5, 5, s.color);
            }
        }
    }
}

void JZChart::drawBarChart(JZPainter& p, const JZRect& plot, double yMin, double yMax) const
{
    int maxN = 0;
    for (const auto& s : m_series)
        maxN = std::max(maxN, (int)s.data.size());
    int sn = (int)m_series.size();
    if (maxN <= 0 || sn <= 0)
        return;

    JZFont valFont(9);
    double slotW = (double)plot.width() / maxN;
    double groupW = slotW * 0.7;
    double barW = groupW / sn;
    int yZero = yAt(0.0, yMin, yMax, plot);

    for (int i = 0; i < maxN; i++) {
        double slotLeft = plot.left() + slotW * i;
        for (int si = 0; si < sn; si++) {
            const Series& s = m_series[si];
            if (i >= (int)s.data.size())
                continue;
            double v = s.data[i];
            int y1 = yAt(v, yMin, yMax, plot);
            int top = std::min(yZero, y1);
            int hgt = std::abs(yZero - y1);
            if (hgt < 1) hgt = 1;
            int x = (int)(slotLeft + (slotW - groupW) * 0.5 + barW * si + 0.5);
            int bw = std::max(1, (int)(barW + 0.5) - 1);
            p.fillRect(x, top, bw, hgt, s.color);

            if (m_showValue && barW > 14) {
                std::string txt = fmtValue(v);
                int tw = JZFontMetrics::textWidth(txt, valFont);
                int ty = top - valFont.size() - 1;
                if (ty < plot.top()) ty = top + 1;
                p.setPen(JZPen(JZColor(0x44, 0x44, 0x44), 1));
                p.setFont(valFont);
                p.drawText(x + bw / 2 - tw / 2, ty, txt.c_str());
            }
        }
    }
}

void JZChart::drawPieChart(JZPainter& p, const JZRect& area) const
{
    if (m_series.empty())
        return;
    const std::vector<double>& data = m_series[0].data;
    int n = (int)data.size();
    if (n == 0)
        return;

    std::vector<std::string> labels;
    std::vector<JZColor> colors;
    double total = 0;
    char buf[24];
    for (int i = 0; i < n; i++) {
        if (i < (int)m_categories.size())
            labels.push_back(m_categories[i]);
        else {
            snprintf(buf, sizeof(buf), "Slice %d", i + 1);
            labels.push_back(buf);
        }
        colors.push_back(paletteColor(i));
        if (data[i] > 0)
            total += data[i];
    }
    if (total <= 0)
        return;

    JZFont legendFont(11);
    int legendW = m_showLegend ? legendWidthOf(labels, legendFont) : 0;

    JZRect pieArea(area.left() + 8, area.top() + 8,
                   area.width() - legendW - 16, area.height() - 16);
    int r = std::min(pieArea.width(), pieArea.height()) / 2 - 4;
    if (r < 8)
        return;
    int cx = pieArea.left() + pieArea.width() / 2;
    int cy = pieArea.top() + pieArea.height() / 2;

    JZFont pctFont(10);
    double start = -90;   // 12 点钟方向起, 顺时针
    for (int i = 0; i < n; i++) {
        if (data[i] <= 0)
            continue;
        double sweep = data[i] / total * 360.0;
        double a0 = start, a1 = start + sweep;

        JZPainterPath path;
        path.moveTo(cx, cy);
        double rad0 = a0 * kPi / 180.0;
        path.lineTo(cx + r * std::cos(rad0), cy + r * std::sin(rad0));
        int steps = std::max(2, (int)(sweep / 3.0));
        for (int k = 1; k <= steps; k++) {
            double ang = (a0 + sweep * k / steps) * kPi / 180.0;
            path.lineTo(cx + r * std::cos(ang), cy + r * std::sin(ang));
        }
        path.closeSubpath();

        p.setBrush(JZBrush(colors[i]));
        p.setPen(JZPen(colors[i], 1));
        p.fillPath(path);
        // 白色分隔线
        p.setPen(JZPen(JZColor(0xff, 0xff, 0xff), 1));
        p.drawPath(path);

        if (m_showValue && sweep >= 18.0) {
            double mid = (a0 + a1) * 0.5 * kPi / 180.0;
            int tx = (int)(cx + r * 0.6 * std::cos(mid));
            int ty = (int)(cy + r * 0.6 * std::sin(mid));
            char pbuf[16];
            snprintf(pbuf, sizeof(pbuf), "%d%%", (int)(data[i] / total * 100 + 0.5));
            std::string txt(pbuf);
            int tw = JZFontMetrics::textWidth(txt, pctFont);
            p.setPen(JZPen(JZColor(0xff, 0xff, 0xff), 1));
            p.setFont(pctFont);
            p.drawText(tx - tw / 2, ty - pctFont.size() / 2, txt.c_str());
        }
        start = a1;
    }

    if (legendW > 0) {
        JZRect la(area.right() - legendW, area.top() + 8, legendW, area.height() - 16);
        drawLegend(p, la, labels, colors);
    }
}

void JZChart::drawLegend(JZPainter& p, const JZRect& area,
                         const std::vector<std::string>& names,
                         const std::vector<JZColor>& colors) const
{
    if (names.empty())
        return;

    JZFont font(11);
    const int swatchW = 14, gap = 6, rowH = 18;
    int x = area.left() + 4;
    int y = area.top() + 4;

    for (size_t i = 0; i < names.size(); i++) {
        int rowTop = y + (int)i * rowH;
        int cy = rowTop + rowH / 2;
        if (cy > area.bottom())
            break;

        JZColor c = i < colors.size() ? colors[i] : JZColor(JZColor::Gray);
        p.setPen(JZPen(c, 2));
        p.drawLine(x, cy, x + swatchW, cy);

        p.setPen(JZPen(JZColor(0x33, 0x33, 0x33), 1));
        p.setFont(font);
        p.drawText(x + swatchW + gap, rowTop + (rowH - font.size()) / 2, names[i].c_str());
    }
}

int JZChart::xAt(int i, int n, const JZRect& plot)
{
    if (n <= 1)
        return plot.left() + plot.width() / 2;
    return plot.left() + (int)(0.5 + (double)i / (n - 1) * plot.width());
}

// 数值 X -> 像素 (X = v 在 [xMin, xMax] 里的位置)
int JZChart::xAtValue(double v, double xMin, double xMax, const JZRect& plot)
{
    if (xMax <= xMin)
        return plot.left() + plot.width() / 2;
    double t = (v - xMin) / (xMax - xMin);
    return plot.left() + (int)(0.5 + t * plot.width());
}

int JZChart::yAt(double v, double yMin, double yMax, const JZRect& plot)
{
    double t = (v - yMin) / (yMax - yMin);
    return plot.bottom() - (int)(0.5 + t * plot.height());
}

std::string JZChart::fmtValue(double v)
{
    char buf[32];
    double a = std::fabs(v);
    if (a >= 100.0)        snprintf(buf, sizeof(buf), "%.0f", v);
    else if (a >= 10.0)    snprintf(buf, sizeof(buf), "%.1f", v);
    else                   snprintf(buf, sizeof(buf), "%.2f", v);
    return std::string(buf);
}
