#ifndef JZ_CHART_H_
#define JZ_CHART_H_

#include "JZWidget.h"
#include "JZColor.h"
#include "JZRect.h"
#include <string>
#include <vector>

// 简易图表控件: 支持折线图 / 柱状图 / 饼图三种类型。
//   - 折线图 / 柱状图: 可含多条序列 (X 按索引或分类标签 0..n-1)。
//   - 饼图: 仅使用第 1 条序列的值作为各扇区占比; 扇区名称取分类标签。
// 自动缩放 Y 轴, 绘制坐标轴/网格/标题/图例。
//
// 典型用法:
//   auto* chart = new JZChart(parent);
//   chart->setChartType(JZChart::BarChart);
//   chart->setCategories({ "一", "二", "三" });
//   int s = chart->addSeries("销量", JZColor(0x33, 0x99, 0x33));
//   chart->setSeriesData(s, { 12, 18, 15 });
class JZChart : public JZWidget
{
    JZ_OBJECT_DECLARE(JZChart)

public:
    enum ChartType {
        LineChart,   // 折线图 (默认)
        BarChart,    // 柱状图
        PieChart,    // 饼图
    };

    // 单条数据序列: 名称 + 颜色 + 数值 (X 坐标按索引 0..n-1)
    struct Series {
        std::string name;
        JZColor color;
        std::vector<double> data;
    };

    explicit JZChart(JZWidget* parent = nullptr);
    ~JZChart();

    JZSize sizeHint() const override;

    // ---- 类型 ----
    void setChartType(ChartType type);
    ChartType chartType() const;

    // ---- 序列管理 ----
    int addSeries(const std::string& name, const JZColor& color = JZColor(JZColor::Blue));
    void setSeriesData(int index, const std::vector<double>& data);
    void setSeriesName(int index, const std::string& name);
    void setSeriesColor(int index, const JZColor& color);
    void clearSeries();
    int seriesCount() const;
    const Series& series(int index) const;
    Series& series(int index);

    // 便捷: 一次性设置多条序列 (各自独立配色, 名称默认 "Series N")
    void setData(const std::vector<std::vector<double>>& seriesList);

    // ---- 分类标签 (折线/柱状的 X 轴标签; 饼图的扇区名称) ----
    void setCategories(const std::vector<std::string>& categories);
    const std::vector<std::string>& categories() const;

    // ---- 外观 ----
    void setTitle(const std::string& title);
    const std::string& title() const;

    void setShowGrid(bool on);
    void setShowLegend(bool on);
    void setShowAxis(bool on);
    void setShowPoint(bool on);   // 折线图是否绘制数据点
    void setShowValue(bool on);   // 柱状图/饼图是否显示数值 (饼图为百分比)

    // 第 i 个默认调色板颜色 (序列/扇区未显式指定颜色时使用)
    static JZColor paletteColor(int i);

protected:
    void paintEvent(JZPainterEvent* event) override;

    void computeRange(double& yMin, double& yMax) const;
    void drawAxis(JZPainter& p, const JZRect& plot, double yMin, double yMax) const;
    void drawLineChart(JZPainter& p, const JZRect& plot, double yMin, double yMax) const;
    void drawBarChart(JZPainter& p, const JZRect& plot, double yMin, double yMax) const;
    void drawPieChart(JZPainter& p, const JZRect& area) const;
    void drawLegend(JZPainter& p, const JZRect& area, const std::vector<std::string>& names,
                    const std::vector<JZColor>& colors) const;

    static int xAt(int i, int n, const JZRect& plot);
    static int yAt(double v, double yMin, double yMax, const JZRect& plot);
    static std::string fmtValue(double v);

    std::vector<Series> m_series;
    std::vector<std::string> m_categories;
    std::string m_title;
    ChartType m_type = LineChart;
    bool m_showGrid = true;
    bool m_showLegend = true;
    bool m_showAxis = true;
    bool m_showPoint = true;
    bool m_showValue = false;
};

#endif // !JZ_CHART_H_
