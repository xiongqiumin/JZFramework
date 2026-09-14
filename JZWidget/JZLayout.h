#ifndef JZ_WIDGET_LAYOUT_H_
#define JZ_WIDGET_LAYOUT_H_
#include <vector>
#include <cstddef>
#include "JZSize.h"
#include "JZObjectPointer.h"

class JZWidget;

class JZLayout
{
public:
    virtual ~JZLayout();

    // 应用到目标容器 (由 JZWidget::setLayout 调用): 保存容器供后续 updateLayout
    // 使用, 并把已登记的管理控件自动挂到容器下 (支持"先 addWidget 再 setLayout"
    // 的写法, 无需加控件前逐个 setParent), 最后重排。
    void apply(JZWidget* container);
    // 已被 apply 的容器控件(尚未 setLayout 时为 nullptr)。
    // 供"往布局里加控件"时定位父控件: 控件进布局后由**容器对象树**持有, 父控件就是它
    // (JS 绑定要用它做"子控件 JS 包装保活"的登记, 见 JZJSWidgetModule::takeWidget)。
    JZWidget* container() const { return m_container; }
    virtual void updateLayout() = 0;   // 子类实现: 把当前 items 排到 m_target
    virtual JZSize sizeHint() const { return JZSize(0, 0); }  // 容器期望尺寸: 内容 hints 之和
    // 容器最小尺寸: 子控件 minimum 尺寸之和。供 JZWidget::minimumSizeHint 使用,
    // 使窗口拖动缩放时下限自动贴合内容, 无需显式 setMinimumSize。
    virtual JZSize minimumSizeHint() const { return JZSize(0, 0); }

    // 从布局中摘除控件 / 清空。布局不拥有控件: 不 delete widget, 只解除管理关系
    // (控件仍以容器为父时保留当前位置继续显示)。JZWidget 析构时自动调用
    // removeWidget, 防止布局 items 持有悬垂指针。
    virtual void removeWidget(JZWidget* widget) { (void)widget; }
    virtual void clear() {}

    int spacing() const { return m_gap; }
    void setSpacing(int gap) { m_gap = gap; updateLayout(); }

    void setContentsMargins(int left, int top, int right, int bottom) {
        m_marginLeft = left; m_marginTop = top; m_marginRight = right; m_marginBottom = bottom;
        updateLayout();
    }
    void getContentsMargins(int* left, int* top, int* right, int* bottom) const {
        if (left)   *left   = m_marginLeft;
        if (top)    *top    = m_marginTop;
        if (right)  *right  = m_marginRight;
        if (bottom) *bottom = m_marginBottom;
    }

protected:

    // 子类在 add 控件后登记、remove 时注销; 布局析构时自动解除全部登记,
    // 保证布局先于控件销毁时, 控件不会反向引用到已销毁的布局。
    void addManaged(JZWidget* widget);
    void removeManaged(JZWidget* widget);

    // 重排容器自身子控件, 并向上级联(容器 sizeHint 因增删 item/改约束而变,
    // 父布局需按新 sizeHint 重排)。等价于 JZWidget::updateGeometry() 的容器侧入口。
    void relayout();

    JZWidget* m_container = nullptr;  // 已被 apply 过的容器
    std::vector<JZObjectPointer> m_managed; // 被本布局管理的控件 (用 JZObjectPointer
                                            // 跟踪存活; 控件销毁后指针自动失效, 布局时
                                            // 由 purgeInvalid 剔除, 避免悬垂登记)

protected:
    // 剔除 m_managed 中已销毁的控件 (兜底: 正常情况下 removeWidget 已自清,
    // 这里防止任何漏注销导致的悬垂登记)。在每次 relayout 入口调用。
    void purgeInvalid();
    // m_items 中的原始指针是否仍由本布局有效持有 (控件存活且已登记)。
    // 用于在 updateLayout/sizeHint 中跳过已销毁但未及时 remove 的 item。
    bool isManagedLive(JZWidget* w) const;
    int m_gap = 0;
    int m_marginLeft = 0;
    int m_marginTop = 0;
    int m_marginRight = 0;
    int m_marginBottom = 0;
};

class JZBoxLayout : public JZLayout
{
public:
    enum ItemType {
        WidgetItem,
        SpacerItem,
        StretchItem
    };

    struct Item {
        ItemType type;
        JZWidget* widget = nullptr;
        int stretch = 0;
        int spaceSize = 0;
    };

    JZBoxLayout(int pad = 0, int spacing = 0);
    ~JZBoxLayout() override;

    JZSize sizeHint() const override;
    JZSize minimumSizeHint() const override;

    void addWidget(JZWidget* widget, int stretch = 0);
    void addSpacing(int size);
    void addStretch(int stretch = 1);

    void insertWidget(int index, JZWidget* widget, int stretch = 0);
    void insertSpacing(int index, int size);
    void insertStretch(int index, int stretch = 1);

    int count() const { return (int)m_items.size(); }
    void removeAt(int index);
    void clear() override;
    void removeWidget(JZWidget* widget) override;

protected:
    void updateLayout() override;
    virtual int flexFlow() const = 0;

    std::vector<Item> m_items;
};

class JZHBoxLayout : public JZBoxLayout
{
public:
    using JZBoxLayout::JZBoxLayout;
protected:
    int flexFlow() const override { return 0; }
};

class JZVBoxLayout : public JZBoxLayout
{
public:
    using JZBoxLayout::JZBoxLayout;
protected:
    int flexFlow() const override { return 1; }
};

class JZGridLayout : public JZLayout
{
public:
    struct Item {
        JZWidget* widget = nullptr;
        int row = 0;
        int col = 0;
        int rowSpan = 1;
        int colSpan = 1;
        int colAlign = 0;
        int rowAlign = 0;
    };

    struct LineSpec {
        int stretch = 0;
        int minSize = -1;
    };

    JZGridLayout(int pad = 0, int gap = 0);
    ~JZGridLayout() override = default;

    void addWidget(JZWidget* widget, int row, int col);
    void addWidget(JZWidget* widget, int fromRow, int fromCol, int rowSpan, int colSpan);

    void setRowStretch(int row, int stretch);
    void setColumnStretch(int col, int stretch);
    void setRowMinimumHeight(int row, int minHeight);
    void setColumnMinimumWidth(int col, int minWidth);

    void setHorizontalSpacing(int spacing);
    void setVerticalSpacing(int spacing);
    int horizontalSpacing() const { return m_hGap >= 0 ? m_hGap : m_gap; }
    int verticalSpacing() const { return m_vGap >= 0 ? m_vGap : m_gap; }

    int rowCount() const;
    int columnCount() const;
    int count() const { return (int)m_items.size(); }

    void clear() override;
    void removeWidget(JZWidget* widget) override;

    JZSize sizeHint() const override;
    JZSize minimumSizeHint() const override;

protected:
    void updateLayout() override;

private:
    std::vector<Item> m_items;
    std::vector<LineSpec> m_colSpecs;
    std::vector<LineSpec> m_rowSpecs;
    int m_hGap = -1;
    int m_vGap = -1;
};


class JZSelfLayout
{
public:
    // 九宫格锚点: 子控件自身同位置点 对齐 容器内容区对应点, 再偏移。
    // 如 TopLeft = 子控件左上角对齐内容区左上角。
    enum Anchor {
        TopLeft = 0, TopCenter, TopRight,
        MiddleLeft, Center, MiddleRight,
        BottomLeft, BottomCenter, BottomRight
    };

    JZSelfLayout();
    ~JZSelfLayout();

    void setAnchor(Anchor anchor, int dx, int dy);
    
protected:    
    Anchor m_anchor;
    int m_dx;
    int m_dy;
};

#endif // JZ_WIDGET_LAYOUT_H_
