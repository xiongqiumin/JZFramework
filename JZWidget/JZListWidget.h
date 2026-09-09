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
#include <functional>

class JZListWidget;

// 列表项数据(与 LVGL 控件解耦, 虚拟列表只持有数据, 不持有任何 LVGL 对象)
class JZListWidgetItem
{
public:
    JZListWidget* widget = nullptr;   // 所属列表(由 addItem 自动设置)
    JZImage icon;                     // 图标(可为空)
    std::string text;                 // 文本
    JZVariant data;                   // 用户自定义数据
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

    // ---- 数据模型 ----
    void addItem(const std::string& text);
    void addItem(const JZListWidgetItem& item);
    void addItems(const std::vector<std::string>& texts);
    void clear();

    int count() const;
    const JZListWidgetItem& item(int index) const;
    std::string itemText(int index) const;
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

jz_signals:
    JZSigInt currentIndexChanged;
    JZSigInt sigActivated;   // 激活 (点击项 / Enter), 下拉弹层收编用

public:
    // ---- AI 语义 ----
    std::string aiRole() const override { return "list"; }
    std::string aiText() const override { return itemText(m_current); }
    std::vector<std::string> aiItems() const override
    {
        std::vector<std::string> v;
        for (const auto& it : m_items)
            v.push_back(it.text);
        return v;
    }
    std::vector<JZAiAction> aiActions() const override { return {{"select", "number 项索引"}}; }

protected:
    void showEvent(JZShowEvent* e) override;
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void keyPressEvent(JZKeyEvent* event) override;

    void updateContentSize() override;
    void defaultPaint(JZPainter& p, const JZRect& rect, JZListWidgetItem* item, int index);
    int itemsPerRow() const;
    int rowHeight() const;
    int cellWidth() const;

    ViewMode m_mode = ListMode;
    std::vector<JZListWidgetItem> m_items;

    int m_itemHeight = 28;     // 列表模式行高
    int m_cellW = 72;          // 图片模式单元格宽
    int m_cellH = 92;          // 图片模式单元格高(图 + 文字)
    int m_current = -1;

    PaintFn m_paintFn;
};

#endif
