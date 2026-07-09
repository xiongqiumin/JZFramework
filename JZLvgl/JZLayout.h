#ifndef JZ_LVGL_LAYOUT_H_
#define JZ_LVGL_LAYOUT_H_

#include "lvgl.h"
#include <vector>
#include <cstddef>

class JZWidget;

class JZLayout
{
public:
    virtual ~JZLayout() = default;
    virtual void apply(lv_obj_t* obj) const = 0;

    int spacing() const { return m_gap; }
    void setSpacing(int gap) { m_gap = gap; }

    void setContentsMargins(int left, int top, int right, int bottom) {
        m_marginLeft = left; m_marginTop = top; m_marginRight = right; m_marginBottom = bottom;
    }
    void getContentsMargins(int* left, int* top, int* right, int* bottom) const {
        if (left)   *left   = m_marginLeft;
        if (top)    *top    = m_marginTop;
        if (right)  *right  = m_marginRight;
        if (bottom) *bottom = m_marginBottom;
    }

protected:
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
        lv_obj_t* spacer = nullptr;
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

    void applyTo(lv_obj_t* obj, lv_flex_flow_t flow) const;

protected:
    mutable std::vector<Item> m_items;

private:
    void clearSpacers();
};

class JZHBoxLayout : public JZBoxLayout
{
public:
    using JZBoxLayout::JZBoxLayout;
    void apply(lv_obj_t* obj) const override;
};

class JZVBoxLayout : public JZBoxLayout
{
public:
    using JZBoxLayout::JZBoxLayout;
    void apply(lv_obj_t* obj) const override;
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
        lv_grid_align_t colAlign = LV_GRID_ALIGN_STRETCH;
        lv_grid_align_t rowAlign = LV_GRID_ALIGN_STRETCH;
    };

    struct LineSpec {
        int stretch = 0;
        lv_coord_t minSize = -1;
    };

    JZGridLayout(int pad = 0, int gap = 0);
    ~JZGridLayout() override = default;

    void addWidget(JZWidget* widget, int row, int col);
    void addWidget(JZWidget* widget, int fromRow, int fromCol, int rowSpan, int colSpan);

    void setRowStretch(int row, int stretch);
    void setColumnStretch(int col, int stretch);
    void setRowMinimumHeight(int row, lv_coord_t minHeight);
    void setColumnMinimumWidth(int col, lv_coord_t minWidth);

    void setHorizontalSpacing(int spacing);
    void setVerticalSpacing(int spacing);
    int horizontalSpacing() const { return m_hGap >= 0 ? m_hGap : m_gap; }
    int verticalSpacing() const { return m_vGap >= 0 ? m_vGap : m_gap; }

    int rowCount() const;
    int columnCount() const;
    int count() const { return (int)m_items.size(); }

    void apply(lv_obj_t* obj) const override;

private:
    void buildColDesc(std::vector<lv_coord_t>& out) const;
    void buildRowDesc(std::vector<lv_coord_t>& out) const;

    std::vector<Item> m_items;
    std::vector<LineSpec> m_colSpecs;
    std::vector<LineSpec> m_rowSpecs;
    int m_hGap = -1;
    int m_vGap = -1;
};

#endif // JZ_LVGL_LAYOUT_H_
