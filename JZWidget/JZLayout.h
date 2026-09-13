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

// 锚定布局 (Self 布局): 子控件按九宫格锚点 + 偏移钉在容器内容区上,
// 容器每次几何变动 (父布局 setGeometry / 手动 resize) 都会经 updateLayout
// 链自动把各子控件摆回相对锚点的位置 —— 无需在容器 resizeEvent 里逐个
// move。适合"悬浮右上角、贴右下角留边距、水平/垂直居中"这类需要相对
// 容器保持稳定的手工定位场景。
//
// 偏移语义 (dx/dy):
//   位于四条边上的锚点 (Top*/Middle*/Bottom*): 正偏移把控件推向内容区
//   中心, 等价"向内边距"; 负值向容器外。
//   正好在中线上的锚点 (TopCenter / MiddleLeft / Center / MiddleRight /
//   BottomCenter): 该轴上没有"内外"之分, 偏移按屏幕轴直接平移
//   (x 正向右、y 正向下)。
//
// 尺寸语义: 布局不改变子控件尺寸 (取 sizeHint 夹 [min,max]), 也不根据
// 内容计算容器期望尺寸 (摆放依赖容器最终尺寸, 见 sizeHint)。若本布局所在
// 容器还要放进父 Box 等布局, 请显式 setFixedSize / 让它 stretch。
class JZSelfLayout : public JZLayout
{
public:
    // 九宫格锚点: 子控件自身同位置点 对齐 容器内容区对应点, 再偏移。
    // 如 TopLeft = 子控件左上角对齐内容区左上角。
    enum Anchor {
        TopLeft = 0, TopCenter, TopRight,
        MiddleLeft, Center, MiddleRight,
        BottomLeft, BottomCenter, BottomRight
    };

    struct Item {
        JZWidget* widget = nullptr;
        Anchor anchor = TopLeft;
        int dx = 0;   // 水平偏移 (靠边锚点: 向内容中心为正; 中线锚点: 按屏幕轴)
        int dy = 0;   // 垂直偏移 (同上)
    };

    JZSelfLayout(int pad = 0);
    ~JZSelfLayout() override = default;

    // 登记/更新子控件锚点 (子控件父必须就是容器; 重复 addWidget 仅改锚点参数)
    void addWidget(JZWidget* widget, Anchor anchor = TopLeft, int dx = 0, int dy = 0);
    void setAnchor(JZWidget* widget, Anchor anchor, int dx = 0, int dy = 0);

    int count() const { return (int)m_items.size(); }
    void removeAt(int index);
    void removeWidget(JZWidget* widget) override;
    void clear() override;

    JZSize sizeHint() const override;
    JZSize minimumSizeHint() const override;

protected:
    void updateLayout() override;

private:
    std::vector<Item> m_items;
};

#endif // JZ_WIDGET_LAYOUT_H_
