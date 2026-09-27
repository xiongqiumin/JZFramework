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
class JZFRAMEWORK_API JZChart : public JZWidget
{
    JZ_OBJECT_DECLARE(JZChart)

public:
    enum ChartType {
        LineChart,   // 折线图 (默认)
        BarChart,    // 柱状图
        PieChart,    // 饼图
    };

    // 单条数据序列: 名称 + 颜色 + 数值 (X 坐标默认取索引 0..n-1, 见下面的 keys)
    struct Series {
        std::string name;
        JZColor color;
        std::vector<double> data;
        // 可选: **数值 X**(与 data 一一对应, 长度应与 data 相同)。
        //   空    = 索引模式: X 取 0..n-1 (原来的行为, 分类标签也按索引对齐);
        //   非空  = 数值模式: X 用这里的值 —— 曲线图要画"电压-容量""电流-时间"这类
        //           X 不是序号的图, 就必须给 keys(等价 qcustomplot 的 QCPGraph key/value 对)。
        std::vector<double> keys;
        // 该序列画在哪条 Y 轴上: 0 = 左轴(默认), 1 = 右轴。
        // 只有真的有用到右轴的序列时才画右轴与右刻度(等价 qcustomplot 的 yAxis2)。
        int valueAxis = 0;
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

    // ---- 数值 X 轴 (区间图必需; 等价 qcustomplot 的 key 轴) ----
    // setSeriesKeys: 只给 X(不改 data); setSeriesDataXY: X/Y 一起给(长度不一致时按短的截断)。
    void setSeriesKeys(int index, const std::vector<double>& keys);
    void setSeriesDataXY(int index, const std::vector<double>& keys, const std::vector<double>& values);
    const std::vector<double>& seriesKeys(int index) const;
    // 序列是否处于数值 X 模式(keys 非空且与 data 等长)
    bool hasSeriesKeys(int index) const;

    // ---- 右 Y 轴 (等价 qcustomplot 的 yAxis2) ----
    void setSeriesValueAxis(int index, int axis);   // 0=左 1=右
    int seriesValueAxis(int index) const;
    bool hasValueAxis(int axis) const;              // 有序列用这条轴(决定要不要画右轴)

    // ---- 轴标题 (等价 qcustomplot 的 setLabel) ----
    // 不用的轴传空串。xLabel 画在 X 轴下方居中; yLabel 画在左轴上方; y2Label 画在右轴上方。
    void setAxisLabels(const std::string& xLabel,
                       const std::string& yLabel,
                       const std::string& y2Label = std::string());
    const std::string& xAxisLabel() const { return m_xLabel; }
    const std::string& yAxisLabel() const { return m_yLabel; }
    const std::string& y2AxisLabel() const { return m_y2Label; }

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

    void computeRange(double& yMin, double& yMax) const;              // 左轴(兼容入口)
    void computeRangeFor(int axis, double& yMin, double& yMax) const; // 指定 Y 轴的值域
    void computeXRange(double& xMin, double& xMax, bool& numeric) const;  // X 值域 + 是否数值模式
    void drawAxis(JZPainter& p, const JZRect& plot,
                  double yMin, double yMax, double y2Min, double y2Max, bool rightAxis,
                  double xMin, double xMax, bool numericX) const;
    void drawLineChart(JZPainter& p, const JZRect& plot,
                       double yMin, double yMax, double y2Min, double y2Max,
                       double xMin, double xMax, bool numericX) const;
    void drawBarChart(JZPainter& p, const JZRect& plot, double yMin, double yMax) const;
    void drawPieChart(JZPainter& p, const JZRect& area) const;
    void drawLegend(JZPainter& p, const JZRect& area, const std::vector<std::string>& names,
                    const std::vector<JZColor>& colors) const;

    static int xAt(int i, int n, const JZRect& plot);
    static int xAtValue(double v, double xMin, double xMax, const JZRect& plot);
    static int yAt(double v, double yMin, double yMax, const JZRect& plot);
    static std::string fmtValue(double v);

    std::vector<Series> m_series;
    std::vector<std::string> m_categories;
    std::string m_title;
    std::string m_xLabel;      // X 轴标题(setAxisLabels)
    std::string m_yLabel;      // 左 Y 轴标题
    std::string m_y2Label;     // 右 Y 轴标题
    ChartType m_type = LineChart;
    bool m_showGrid = true;
    bool m_showLegend = true;
    bool m_showAxis = true;
    bool m_showPoint = true;
    bool m_showValue = false;
};

#endif // !JZ_CHART_H_
