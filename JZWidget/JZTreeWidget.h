#ifndef JZ_TREE_WIDGET_H_
#define JZ_TREE_WIDGET_H_

#include "JZAbstractViewWidget.h"
#include "JZSignal.h"
#include "JZImage.h"
#include "JZVariant.h"
#include "JZPainter.h"
#include "JZRect.h"
#include <vector>
#include <string>
#include <functional>

class JZTreeWidget;

// 树节点数据(与 LVGL 控件解耦, owner-draw 虚拟树只持有数据, 不持有任何 LVGL 对象)。
// 节点由 JZTreeWidget 拥有并管理生命周期: 通过 addTopLevelItem/addChild 加入后,
// 由树统一在 clear()/析构/被 take 时删除。
class JZTreeWidgetItem
{
public:
    JZTreeWidgetItem();
    explicit JZTreeWidgetItem(const std::string& text);
    ~JZTreeWidgetItem();

    JZTreeWidget* treeWidget() const { return m_widget; }
    JZTreeWidgetItem* parent() const { return m_parent; }

    void setText(const std::string& text);
    std::string text() const { return m_text; }
    void setIcon(const JZImage& icon);
    JZImage icon() const { return m_icon; }
    void setData(const JZVariant& data) { m_data = data; }
    JZVariant data() const { return m_data; }

    // 子节点(所有权随父节点转移给树)
    void addChild(JZTreeWidgetItem* child);
    void insertChild(int index, JZTreeWidgetItem* child);
    JZTreeWidgetItem* takeChild(int index);          // 取出, 调用方获得所有权
    void removeChild(JZTreeWidgetItem* child);
    int childCount() const { return (int)m_children.size(); }
    JZTreeWidgetItem* child(int index) const;
    int childIndex() const;                          // 自身在父节点中的索引(top-level 返回 -1)
    int indexOfChild(const JZTreeWidgetItem* child) const;

    bool isExpanded() const { return m_expanded; }
    void setExpanded(bool exp);                       // 折叠态由树维护可见行, 故经由树通知

    int depth() const { return m_depth; }             // 由树在 rebuildVisible 时回填

private:
    void setWidget(JZTreeWidget* w);
    void setDepth(int d) { m_depth = d; }

    JZTreeWidget* m_widget = nullptr;
    JZTreeWidgetItem* m_parent = nullptr;
    std::vector<JZTreeWidgetItem*> m_children;
    std::string m_text;
    JZImage m_icon;
    JZVariant m_data;
    bool m_expanded = true;
    int m_depth = 0;

    friend class JZTreeWidget;
};

// 虚拟树(owner-draw): 仅绘制可见行, 通过 JZPainter 直接画到画布,
// 不创建/复用任何行控件。支持多级缩进、展开/折叠、单选。
class JZTreeWidget : public JZAbstractViewWidget
{
    JZ_OBJECT_DECLARE(JZTreeWidget)

    friend class JZTreeWidgetItem;   // 允许节点回调树的 protected 方法(rebuildVisible 等)

public:
    explicit JZTreeWidget(JZWidget* parent = nullptr);
    ~JZTreeWidget();

    // ---- 数据模型 ----
    void addTopLevelItem(JZTreeWidgetItem* item);
    void addTopLevelItems(const std::vector<JZTreeWidgetItem*>& items);
    void insertTopLevelItem(int index, JZTreeWidgetItem* item);
    JZTreeWidgetItem* takeTopLevelItem(int index);          // 取出顶层项, 调用方获得所有权
    int topLevelItemCount() const { return (int)m_topItems.size(); }
    JZTreeWidgetItem* topLevelItem(int index) const;
    int indexOfTopLevelItem(JZTreeWidgetItem* item) const;
    void clear();                                            // 删除所有项

    JZTreeWidgetItem* itemAbove(JZTreeWidgetItem* item) const;
    JZTreeWidgetItem* itemBelow(JZTreeWidgetItem* item) const;

    // ---- 视图 ----
    void setHeaderLabel(const std::string& label);
    std::string headerLabel() const { return m_header; }
    void setRowHeight(int h);
    int rowHeight() const { return m_rowHeight; }
    void setIndentation(int px);
    int indentation() const { return m_indent; }

    // 自定义绘图 (可选); 不设置则使用内置默认绘图(缩进 + 三角 + 图标 + 文本)
    // rect 为该项在视口中的矩形; expanded 表示当前折叠态(供三角绘制)
    using PaintFn = std::function<void(JZPainter& painter, const JZRect& rect,
                                       JZTreeWidgetItem* item, int depth, bool expanded)>;
    void setItemPaint(PaintFn fn);

    // ---- 选择 ----
    JZTreeWidgetItem* currentItem() const { return m_current; }
    int currentIndex() const;                              // 行索引(可见行)
    void setCurrentItem(JZTreeWidgetItem* item, bool emitSignal = true);

    // ---- 展开/折叠 ----
    void expandAll();
    void collapseAll();
    void expandItem(JZTreeWidgetItem* item);
    void collapseItem(JZTreeWidgetItem* item);

jz_signals:
    JZSignal<JZTreeWidgetItem*> currentItemChanged;
    JZSignal<JZTreeWidgetItem*> itemActivated;             // 激活 (点击/Enter)
    JZSignal<JZTreeWidgetItem*> itemExpanded;
    JZSignal<JZTreeWidgetItem*> itemCollapsed;

    // 供 JZTreeWidgetItem 在数据变化时回调, 触发重绘
    void itemChanged(JZTreeWidgetItem* item);

protected:
    void showEvent(JZShowEvent* e) override;
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void keyPressEvent(JZKeyEvent* event) override;

    void rebuildVisible();                                 // 重算可见行(并回填 depth)
    void updateContentSize() override;
    void defaultPaint(JZPainter& p, const JZRect& rect, JZTreeWidgetItem* item, int depth, bool expanded, bool selected);
    void paintHeader(JZPainter& p);

    int rowCount() const { return (int)m_visible.size(); }
    JZTreeWidgetItem* itemAtRow(int row) const;
    int rowOfItem(JZTreeWidgetItem* item) const;
    void ensureRowVisible(int row);

    struct Row { JZTreeWidgetItem* item; int depth; };
    std::vector<JZTreeWidgetItem*> m_topItems;
    std::vector<Row> m_visible;                            // 缓存的可见行(随结构/折叠变化重算)

    int m_rowHeight = 24;
    int m_indent = 18;                                     // 每层缩进
    int m_toggleZone = 16;                                 // 三角点击区宽度
    std::string m_header;
    JZTreeWidgetItem* m_current = nullptr;

    PaintFn m_paintFn;
};

#endif
