#ifndef JZ_WIDGET_TABLE_H_
#define JZ_WIDGET_TABLE_H_

#include "JZAbstractViewWidget.h"
#include "JZSignal.h"
#include "JZImage.h"
#include "JZVariant.h"
#include "JZPainter.h"
#include "JZRect.h"
#include <vector>
#include <string>
#include <memory>
#include <functional>

class JZTableWidget;

// 单元格数据(与 LVGL 控件解耦, 虚拟表格只持有数据, 不持有任何 LVGL 对象)。
// 所有权: 由 JZTableWidget 持有(unique_ptr 存于 m_grid), 外部拿到的是**借用指针** ——
// 只要不被 takeItem/removeRow/removeColumn/clearContents/表格析构清掉, 地址就稳定
// (改别的格子不会搬移本格, 这点与 JZListWidgetItem 一致)。
// 改值用 setText/setIcon/setData: 会通知所属表格重绘。
class JZTableWidgetItem
{
public:
    JZTableWidgetItem() {}
    explicit JZTableWidgetItem(const std::string& text) : m_text(text) {}

    // 所属表格与所在位置(由 setItem 自动设置; 被摘除后 table() 为 nullptr、行列号为 -1)
    JZTableWidget* table() const { return m_table; }
    int row() const { return m_row; }
    int column() const { return m_col; }

    const std::string& text() const { return m_text; }
    const JZImage& icon() const { return m_icon; }
    const JZVariant& data() const { return m_data; }

    void setText(const std::string& t);
    void setIcon(const JZImage& i);
    void setData(const JZVariant& d);

private:
    friend class JZTableWidget;   // 表格负责设置归属/位置, 并直接读写内部状态

    JZTableWidget* m_table = nullptr;
    int m_row = -1;
    int m_col = -1;
    JZImage m_icon;
    std::string m_text;
    JZVariant m_data;
};

// 虚拟表格(owner-draw): 仅绘制当前可见区域的单元格, 通过 JZPainter 直接画到画布上,
// 不创建/复用任何单元格控件。表头冻结在顶部, 主体可双向滚动。
class JZTableWidget : public JZAbstractViewWidget
{
    JZ_OBJECT_DECLARE(JZTableWidget)

public:
    // 自定义绘图函数: painter 为画布, rect 为单元格在视口中的矩形, item 为数据(空单元格为 nullptr)
    using PaintFn = std::function<void(JZPainter& painter, const JZRect& rect, JZTableWidgetItem* item)>;

    explicit JZTableWidget(JZWidget* parent = nullptr);
    ~JZTableWidget();

    // ---- 数据模型 (接口形态与 Qt QTableWidget 对齐) ----
    // 单元格对象由调用方 new 并交给表格**接管所有权**(/Transfer/); 表格不返回句柄,
    // 需要时用 item(row,col) 取回(借用)。空单元格 = nullptr(Qt 语义)。
    void setItem(int row, int col, JZTableWidgetItem* item);   // 接管; 传 nullptr 清空该格  (Qt: setItem)
    JZTableWidgetItem* item(int row, int col) const;           // 越界或空单元格返回 nullptr   (Qt: item)
    JZTableWidgetItem* takeItem(int row, int col);             // 摘除并交回所有权            (Qt: takeItem)

    void setRowCount(int rows);                                // 收缩时多余的行连同 item 一起删
    int rowCount() const;
    void setColumnCount(int cols);
    int columnCount() const;

    void insertRow(int row);                                   // 插入空行(item 行号顺延)
    void removeRow(int row);                                   // 删除整行(含其 item)
    void insertColumn(int col);
    void removeColumn(int col);

    void clearContents();                                      // 删除全部 item       (Qt: clearContents)
    void clear();                                              // 清 item + 表头标签  (Qt: clear)

    // 查询
    int row(const JZTableWidgetItem* item) const;              // 不在本表返回 -1   (Qt: row(item))
    int column(const JZTableWidgetItem* item) const;           //                  (Qt: column(item))

    // 数据变了(行列增删 / 单元格文本/图标/data / 表头)就发一次 sigDataChanged ——
    // 外部观察者(JS 的 `onDataChanged`、AI 工具)靠它感知"数据被改过了"。
    // 任何改数据的入口(含 JS 绑定层自己发起的那些)都会发, 不做"谁发起"的区分。
    // 只发信号, 不负责重绘/尺寸刷新(调用点各自保留原有行为)。
    void notifyDataChanged();

    // 便捷值访问(等价 item(row,col)->text(): 写时自动建 item, 读时无单元格返回空串)
    void setItemText(int row, int col, const std::string& text);
    std::string itemText(int row, int col) const;
    void setCellValue(int row, int col, const std::string& text);   // 兼容保留, 同 setItemText
    std::string cellValue(int row, int col) const;                  // 兼容保留, 同 itemText

    // ---- 视图 ----
    void setColumnWidth(int col, int w);
    void setHorizontalHeaderLabels(const std::vector<std::string>& labels);
    std::vector<std::string> headerLabels() const;   // 表头标签快照 (setHorizontalHeaderLabels 的读回)
    void setRowHeight(int h);
    int rowHeight() const;
    int headerHeight() const;   // 表头高度(= 行高)

    // 设置自定义绘图函数; 不设置则使用内置默认绘图(图标 + 文本)
    void setItemPaint(PaintFn fn);

    // ---- 选择 ----
    void setCurrentCell(int row, int col);
    int currentRow() const;
    int currentColumn() const;
    JZTableWidgetItem* currentItem() const;                              // 无选中/空单元格返回 nullptr
    void setCurrentItem(JZTableWidgetItem* item, bool emitSignal = true);   // (Qt: setCurrentItem)

jz_signals:
    JZSigInt currentRowChanged;
    JZSigInt currentColumnChanged;
    JZSigVoid sigDataChanged;   // 数据(行列/单元格/表头)变化 —— 外部观察者(JS onDataChanged)用

protected:
    void showEvent(JZShowEvent* e) override;
    void paintEvent(JZPainterEvent* event) override;   // 绘制冻结表头

    void updateContentSize();
    void defaultPaint(JZPainter& p, const JZRect& rect, JZTableWidgetItem* item, int row, int col);
    void paintHeader(JZPainter& p);
    int colX(int col) const;
    int colFromX(int x) const;
    int totalWidth() const;
    void invalidateAll();

    // 内部: 摘除(转移所有权, 不删除) / 落位 / 删除区间 / 行列号重排
    std::unique_ptr<JZTableWidgetItem> detachAt(int row, int col);
    void placeItem(int row, int col, std::unique_ptr<JZTableWidgetItem> item);
    void reindexItems();

    // [row][col], 空单元格为 nullptr。
    // 用 unique_ptr 存放: item 地址稳定(值存 vector 一旦扩容搬移, 外部拿到的借用指针会静默悬垂),
    // 同时所有权/删除点唯一。
    std::vector<std::vector<std::unique_ptr<JZTableWidgetItem>>> m_grid;
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
