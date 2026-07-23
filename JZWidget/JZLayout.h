#ifndef JZ_WIDGET_LAYOUT_H_
#define JZ_WIDGET_LAYOUT_H_
#include <vector>
#include <cstddef>

class JZWidget;

class JZLayout
{
public:
    virtual ~JZLayout() = default;

    // 应用到目标容器 (由 JZWidget::setLayout 调用), 同时保存目标供后续 updateLayout 使用
    void apply(void* obj);
    // 将当前 items / 参数重新布局到目标容器 (增删 / 改属性后调用一次)
    void updateLayout();

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
    virtual void doLayout() = 0;   // 子类实现: 把当前 items 排到 m_target

    void* m_target = nullptr;  // 已被 apply 过的容器
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
        void* spacer = nullptr;
        int stretch = 0;
        int spaceSize = 0;
    };

    JZBoxLayout(int pad = 0, int spacing = 0);
    ~JZBoxLayout() override;

    void addWidget(JZWidget* widget, int stretch = 0);
    void addSpacing(int size);
    void addStretch(int stretch = 1);

    void insertWidget(int index, JZWidget* widget, int stretch = 0);
    void insertSpacing(int index, int size);
    void insertStretch(int index, int stretch = 1);

    int count() const { return (int)m_items.size(); }
    void removeAt(int index);
    void clear();

protected:
    void doLayout() override;
    virtual int flexFlow() const = 0;

    std::vector<Item> m_items;

private:
    void clearSpacers();
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

protected:
    void doLayout() override;

private:
    void buildColDesc(std::vector<int>& out) const;
    void buildRowDesc(std::vector<int>& out) const;

    std::vector<Item> m_items;
    std::vector<LineSpec> m_colSpecs;
    std::vector<LineSpec> m_rowSpecs;
    int m_hGap = -1;
    int m_vGap = -1;
    mutable std::vector<int> m_colDescCache;
    mutable std::vector<int> m_rowDescCache;
};

#endif // JZ_WIDGET_LAYOUT_H_
