#ifndef JZ_LVGL_TABLE_H_
#define JZ_LVGL_TABLE_H_

#include "JZScrollArea.h"
#include "JZImage.h"
#include "JZVariant.h"
#include "JZPainter.h"
#include "JZRect.h"
#include <vector>
#include <string>
#include <functional>

class JZTableWidget;

// 单元格数据(与 LVGL 控件解耦, 虚拟表格只持有数据, 不持有任何 LVGL 对象)
class JZTableWidgetItem
{
public:
    JZTableWidget* widget = nullptr;   // 所属表格(由 setItem 自动设置)
    JZImage icon;                      // 图标(可为空)
    std::string text;                  // 文本
    JZVariant data;                    // 用户自定义数据
};

// 虚拟表格(owner-draw): 仅绘制当前可见区域的单元格, 通过 JZPainter 直接画到画布上,
// 不创建/复用任何单元格控件。表头冻结在顶部, 主体可双向滚动。
class JZTableWidget : public JZScrollArea
{
    JZ_OBJECT_DECLARE(JZTableWidget)

public:
    // 自定义绘图函数: painter 为画布, rect 为单元格在视口中的矩形, item 为数据
    using PaintFn = std::function<void(JZPainter& painter, const JZRect& rect, JZTableWidgetItem* item)>;

    explicit JZTableWidget(JZWidget* parent = nullptr);
    ~JZTableWidget();

    // ---- 数据模型 ----
    void setColumnCount(int cols);
    int columnCount() const;
    void setRowCount(int rows);
    int rowCount() const;

    void setItem(int row, int col, const JZTableWidgetItem& item);
    const JZTableWidgetItem& item(int row, int col) const;

    void setCellValue(int row, int col, const std::string& text);
    std::string cellValue(int row, int col) const;

    // ---- 视图 ----
    void setColumnWidth(int col, int w);
    void setHorizontalHeaderLabels(const std::vector<std::string>& labels);
    void setRowHeight(int h);
    int rowHeight() const;
    int headerHeight() const;   // 表头高度(= 行高)

    // 设置自定义绘图函数; 不设置则使用内置默认绘图(图标 + 文本)
    void setItemPaint(PaintFn fn);

    // ---- 选择 ----
    void setCurrentCell(int row, int col);
    int currentRow() const;
    int currentColumn() const;

jz_signals:
    JZSigInt currentRowChanged;
    JZSigInt currentColumnChanged;

protected:
    void onLvEvent(lv_event_t* e) override;
    void showEvent(JZShowEvent* e) override;
    void resizeEvent(JZResizeEvent* e) override;
    void paintEvent(JZPainterEvent* event) override;   // 绘制冻结表头

    void updateContentSize();
    void defaultPaint(JZPainter& p, const JZRect& rect, JZTableWidgetItem* item, int row, int col);
    void paintHeader(JZPainter& p);
    int colX(int col) const;
    int colFromX(int x) const;
    int totalWidth() const;
    void invalidateAll();

    std::vector<std::vector<JZTableWidgetItem>> m_grid;  // [row][col]
    int m_cols = 0;
    int m_rowHeight = 24;

    std::vector<int> m_colWidths;
    std::vector<int> m_colOffsets;       // 列起始 x(由 updateContentSize 维护)
    std::vector<std::string> m_headerLabels;
    int m_totalW = 0;

    int m_curRow = -1;
    int m_curCol = -1;

    PaintFn m_paintFn;
};

#endif
