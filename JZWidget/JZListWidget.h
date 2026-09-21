#ifndef JZ_LIST_WIDGET_H_
#define JZ_LIST_WIDGET_H_

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

class JZListWidget;

// 列表项数据(与 LVGL 控件解耦, 虚拟列表只持有数据, 不持有任何 LVGL 对象)。
// 所有权: 由 JZListWidget 持有(unique_ptr 存于 m_items), 外部拿到的是**借用指针** ——
// 地址只要不被 remove/take/clear 就一直稳定(增删其它项不会搬移本项), 列表析构后失效。
// 改值用 setText/setIcon/setData: 会通知所属列表重绘。
class JZListWidgetItem
{
public:
    JZListWidgetItem() {}
    explicit JZListWidgetItem(const std::string& text) : m_text(text) {}

    // 所属列表(由 append/insert 自动设置; 未加入列表或被 takeItem 摘除后为 nullptr)
    JZListWidget* widget() const { return m_widget; }

    const std::string& text() const { return m_text; }
    const JZImage& icon() const { return m_icon; }
    const JZVariant& data() const { return m_data; }

    void setText(const std::string& t);
    void setIcon(const JZImage& i);
    void setData(const JZVariant& d);

private:
    friend class JZListWidget;      // 列表负责设置 m_widget 并直接读写内部状态

    JZListWidget* m_widget = nullptr;
    JZImage m_icon;
    std::string m_text;
    JZVariant m_data;
};

// 虚拟列表(owner-draw): 仅绘制当前可见区域, 通过 JZPainter 直接画到画布上,
// 不创建/复用任何行控件。同时支持列表模式(单列: 图标 + 文本)与图片模式(网格)。
class JZListWidget : public JZAbstractViewWidget
{
    JZ_OBJECT_DECLARE(JZListWidget)

public:
    enum ViewMode { ListMode, ImageMode };

    // 自定义绘图函数: painter 为画布, rect 为该项在视口中的矩形, item 为数据
    using PaintFn = std::function<void(JZPainter& painter, const JZRect& rect, JZListWidgetItem* item)>;

    explicit JZListWidget(JZWidget* parent = nullptr);
    ~JZListWidget();

    // ---- 数据模型 (接口形态与 Qt QListWidget 对齐) ----
    // 项对象由调用方创建并交给列表**接管所有权**(/Transfer/); 列表不返回句柄,
    // 需要时用 item(row) 取回。新增接口一律返回 void —— 与 Qt 的 addItem/insertItem 一致。
    // item()/currentItem() 是 const 方法却返回非 const 指针: 与 Qt 的 QListWidget::item(row) const
    // 一致 —— "写项属性"是列表数据的正常用法, 改值由 item 自己通知列表重绘。
    void appendItem(const std::string& text);                   // 列表新建一项  (Qt: addItem(QString))
    void appendItem(JZListWidgetItem* item);                    // 接管, 须 new 分配 (Qt: addItem(item))
    void appendItems(const std::vector<std::string>& texts);    //              (Qt: addItems)
    void insertItem(int index, const std::string& text);        //              (Qt: insertItem(row, QString))
    void insertItem(int index, JZListWidgetItem* item);         // 接管          (Qt: insertItem(row, item))

    // 摘除: 交出所有权, 由调用方释放。Qt 没有 removeItem —— 要删除就是 `delete takeItem(row)`。
    JZListWidgetItem* takeItem(int index);                      // 越界返回 nullptr

    void clear();

    // 数据变了(增删项 / 文本 / 图标 / data)就发一次 sigDataChanged ——
    // 外部观察者(JS 的 `onDataChanged`、AI 工具)靠它感知"数据被改过了"。
    // 任何改数据的入口(含 JS 绑定层自己发起的那些)都会发, 不做"谁发起"的区分。
    // 只发信号, 不做重绘/尺寸刷新(调用点各自负责, 保持原有行为不变)。
    void notifyDataChanged();

    // 查询
    int count() const;
    JZListWidgetItem* item(int index) const;                    // 越界返回 nullptr
    int row(const JZListWidgetItem* item) const;                // 未找到返回 -1 (Qt: row(item))
    std::string itemText(int index) const;                      // 便捷值访问(等价 item(i)->text())
    JZVariant itemData(int index) const;

    // ---- 视图 ----
    void setViewMode(ViewMode mode);
    ViewMode viewMode() const;
    void setItemHeight(int h);          // 列表模式行高
    int itemHeight() const;
    void setCellSize(int w, int h);     // 图片模式单元格大小

    // 期望尺寸: 宽度贴合最宽项 (文本 + 图标), 高度按项数 * 行高 (至多 8 行,
    // 再多交给滚动)。虚拟列表只有自己知道该多大 —— 基类默认 0×0 会让它在
    // 固定项 (stretch=0) 的布局里被压成 0。
    JZSize sizeHint() const override;

    // 设置自定义绘图函数; 不设置则使用内置默认绘图(图标 + 文本)
    void setItemPaint(PaintFn fn);

    // ---- 选择 ----
    int currentIndex() const;
    void setCurrentIndex(int index);
    JZListWidgetItem* currentItem() const;                     // 无选中返回 nullptr
    void setCurrentItem(JZListWidgetItem* item, bool emitSignal = true);

jz_signals:
    // 当前项变化; **-1 = 无选中** (摘除当前项 / clear() 时发一次)。
    // 摘除当前项后不会自动把选择挪到邻项(Qt 语义), 要保持选中由调用方自己 setCurrentIndex。
    JZSigInt currentIndexChanged;
    JZSigInt sigActivated;   // 激活 (点击项 / Enter), 下拉弹层收编用
    JZSigVoid sigDataChanged;   // 数据(增删项/文本/图标/data)变化 —— 外部观察者(JS onDataChanged)用

public:
    // ---- AI 语义 ----
    std::string aiRole() const override { return "list"; }
    std::string aiText() const override { return itemText(m_current); }
    std::vector<std::string> aiItems() const override
    {
        std::vector<std::string> v;
        for (const auto& it : m_items)
            v.push_back(it->text());
        return v;
    }
    std::vector<JZAiAction> aiActions() const override { return {{"select", "number 项索引"}}; }

protected:
    void showEvent(JZShowEvent* e) override;
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void keyPressEvent(JZKeyEvent* event) override;

    void updateContentSize() override;
    // 追加/插入的统一实现: 建立反向引用 -> 落位 -> 刷新尺寸与重绘。
    JZListWidgetItem* insertItemAt(int index, std::unique_ptr<JZListWidgetItem> item);
    void defaultPaint(JZPainter& p, const JZRect& rect, JZListWidgetItem* item, int index);
    int itemsPerRow() const;
    int rowHeight() const;
    int cellWidth() const;

    ViewMode m_mode = ListMode;
    // 用 unique_ptr 存放: 保证 item 地址稳定(值存 vector 一旦扩容搬移, 外部拿到的借用指针
    // 会静默悬垂), 同时所有权/删除点唯一。
    std::vector<std::unique_ptr<JZListWidgetItem>> m_items;

    int m_itemHeight = 28;     // 列表模式行高
    int m_cellW = 72;          // 图片模式单元格宽
    int m_cellH = 92;          // 图片模式单元格高(图 + 文字)
    int m_current = -1;

    PaintFn m_paintFn;
};

#endif
