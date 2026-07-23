#include "JZLayout.h"
#include "JZWidget.h"
#include <algorithm>

// ==================== JZLayout ====================

void JZLayout::apply(lv_obj_t* obj)
{
    m_target = obj;
    updateLayout();
}

void JZLayout::updateLayout()
{
    if (!m_target)
        return;
    doLayout();
}

// ==================== JZBoxLayout ====================

JZBoxLayout::JZBoxLayout(int pad, int spacing)
{
    if (pad > 0)
        setContentsMargins(pad, pad, pad, pad);
    m_gap = spacing;
}

JZBoxLayout::~JZBoxLayout()
{
    clearSpacers();
}

void JZBoxLayout::addWidget(JZWidget* widget, int stretch)
{
    Item it;
    it.type = WidgetItem;
    it.widget = widget;
    it.stretch = stretch < 0 ? 0 : stretch;
    m_items.push_back(it);
    updateLayout();
}

void JZBoxLayout::addSpacing(int size)
{
    Item it;
    it.type = SpacerItem;
    it.spaceSize = size < 0 ? 0 : size;
    m_items.push_back(it);
    updateLayout();
}

void JZBoxLayout::addStretch(int stretch)
{
    Item it;
    it.type = StretchItem;
    it.stretch = stretch <= 0 ? 1 : stretch;
    m_items.push_back(it);
    updateLayout();
}

void JZBoxLayout::insertWidget(int index, JZWidget* widget, int stretch)
{
    Item it;
    it.type = WidgetItem;
    it.widget = widget;
    it.stretch = stretch < 0 ? 0 : stretch;
    if (index < 0 || index >= (int)m_items.size())
        m_items.push_back(it);
    else
        m_items.insert(m_items.begin() + index, it);
    updateLayout();
}

void JZBoxLayout::insertSpacing(int index, int size)
{
    Item it;
    it.type = SpacerItem;
    it.spaceSize = size < 0 ? 0 : size;
    if (index < 0 || index >= (int)m_items.size())
        m_items.push_back(it);
    else
        m_items.insert(m_items.begin() + index, it);
    updateLayout();
}

void JZBoxLayout::insertStretch(int index, int stretch)
{
    Item it;
    it.type = StretchItem;
    it.stretch = stretch <= 0 ? 1 : stretch;
    if (index < 0 || index >= (int)m_items.size())
        m_items.push_back(it);
    else
        m_items.insert(m_items.begin() + index, it);
    updateLayout();
}

void JZBoxLayout::removeAt(int index)
{
    if (index < 0 || index >= (int)m_items.size()) return;
    Item& it = m_items[index];
    if (it.spacer) {
        lv_obj_delete(it.spacer);
        it.spacer = nullptr;
    }
    m_items.erase(m_items.begin() + index);
    updateLayout();
}

void JZBoxLayout::clear()
{
    clearSpacers();
    m_items.clear();
    updateLayout();
}

void JZBoxLayout::clearSpacers()
{
    for (auto& it : m_items) {
        if (it.spacer) {
            lv_obj_delete(it.spacer);
            it.spacer = nullptr;
        }
    }
}

void JZBoxLayout::doLayout()
{
    lv_obj_t* obj = m_target;
    if (!obj) return;

    const bool isRow = (flexFlow() == LV_FLEX_FLOW_ROW);

    lv_obj_set_layout(obj, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(obj, flexFlow());
    if (m_marginLeft >= 0)   lv_obj_set_style_pad_left(obj, m_marginLeft, 0);
    if (m_marginTop >= 0)    lv_obj_set_style_pad_top(obj, m_marginTop, 0);
    if (m_marginRight >= 0)  lv_obj_set_style_pad_right(obj, m_marginRight, 0);
    if (m_marginBottom >= 0) lv_obj_set_style_pad_bottom(obj, m_marginBottom, 0);
    if (m_gap > 0)           lv_obj_set_style_pad_gap(obj, m_gap, 0);

    for (auto& it : m_items) {
        if (it.type == WidgetItem) {
            if (!it.widget) continue;
            lv_obj_t* child = it.widget->handle();
            if (!child) continue;
            lv_obj_set_flex_grow(child, (lv_coord_t)(it.stretch > 0 ? it.stretch : 0));
        } else if (it.type == SpacerItem) {
            if (!it.spacer) {
                it.spacer = lv_obj_create(obj);
                lv_obj_remove_style_all(it.spacer);
            }
            if (isRow) lv_obj_set_size(it.spacer, it.spaceSize, LV_SIZE_CONTENT);
            else       lv_obj_set_size(it.spacer, LV_SIZE_CONTENT, it.spaceSize);
            lv_obj_set_flex_grow(it.spacer, 0);
        } else if (it.type == StretchItem) {
            if (!it.spacer) {
                it.spacer = lv_obj_create(obj);
                lv_obj_remove_style_all(it.spacer);
            }
            if (isRow) lv_obj_set_size(it.spacer, 0, LV_SIZE_CONTENT);
            else       lv_obj_set_size(it.spacer, LV_SIZE_CONTENT, 0);
            lv_obj_set_flex_grow(it.spacer, (lv_coord_t)(it.stretch > 0 ? it.stretch : 1));
        }
    }
}

// ==================== JZGridLayout ====================

JZGridLayout::JZGridLayout(int pad, int gap)
{
    if (pad > 0)
        setContentsMargins(pad, pad, pad, pad);
    m_gap = gap;
}

void JZGridLayout::addWidget(JZWidget* widget, int row, int col)
{
    addWidget(widget, row, col, 1, 1);
}

void JZGridLayout::addWidget(JZWidget* widget, int fromRow, int fromCol, int rowSpan, int colSpan)
{
    Item it;
    it.widget = widget;
    it.row = fromRow;
    it.col = fromCol;
    it.rowSpan = rowSpan > 0 ? rowSpan : 1;
    it.colSpan = colSpan > 0 ? colSpan : 1;
    m_items.push_back(it);
    updateLayout();
}

void JZGridLayout::setRowStretch(int row, int stretch)
{
    if (row < 0) return;
    if ((size_t)row >= m_rowSpecs.size()) m_rowSpecs.resize(row + 1);
    m_rowSpecs[row].stretch = stretch;
    updateLayout();
}

void JZGridLayout::setColumnStretch(int col, int stretch)
{
    if (col < 0) return;
    if ((size_t)col >= m_colSpecs.size()) m_colSpecs.resize(col + 1);
    m_colSpecs[col].stretch = stretch;
    updateLayout();
}

void JZGridLayout::setRowMinimumHeight(int row, lv_coord_t minHeight)
{
    if (row < 0) return;
    if ((size_t)row >= m_rowSpecs.size()) m_rowSpecs.resize(row + 1);
    m_rowSpecs[row].minSize = minHeight;
    updateLayout();
}

void JZGridLayout::setColumnMinimumWidth(int col, lv_coord_t minWidth)
{
    if (col < 0) return;
    if ((size_t)col >= m_colSpecs.size()) m_colSpecs.resize(col + 1);
    m_colSpecs[col].minSize = minWidth;
    updateLayout();
}

void JZGridLayout::setHorizontalSpacing(int spacing)
{
    m_hGap = spacing;
    updateLayout();
}

void JZGridLayout::setVerticalSpacing(int spacing)
{
    m_vGap = spacing;
    updateLayout();
}

int JZGridLayout::rowCount() const
{
    int maxR = (int)m_rowSpecs.size();
    for (const auto& it : m_items) {
        int end = it.row + it.rowSpan;
        if (end > maxR) maxR = end;
    }
    return maxR;
}

int JZGridLayout::columnCount() const
{
    int maxC = (int)m_colSpecs.size();
    for (const auto& it : m_items) {
        int end = it.col + it.colSpan;
        if (end > maxC) maxC = end;
    }
    return maxC;
}

void JZGridLayout::buildColDesc(std::vector<lv_coord_t>& out) const
{
    int n = columnCount();
    out.clear();
    out.reserve(n + 1);
    for (int i = 0; i < n; ++i) {
        lv_coord_t v = LV_SIZE_CONTENT;
        if (i < (int)m_colSpecs.size()) {
            const auto& s = m_colSpecs[i];
            if (s.stretch > 0) v = LV_GRID_FR((lv_coord_t)s.stretch);
            else if (s.minSize >= 0) v = s.minSize;
        }
        out.push_back(v);
    }
    out.push_back(LV_GRID_TEMPLATE_LAST);
}

void JZGridLayout::buildRowDesc(std::vector<lv_coord_t>& out) const
{
    int n = rowCount();
    out.clear();
    out.reserve(n + 1);
    for (int i = 0; i < n; ++i) {
        lv_coord_t v = LV_SIZE_CONTENT;
        if (i < (int)m_rowSpecs.size()) {
            const auto& s = m_rowSpecs[i];
            if (s.stretch > 0) v = LV_GRID_FR((lv_coord_t)s.stretch);
            else if (s.minSize >= 0) v = s.minSize;
        }
        out.push_back(v);
    }
    out.push_back(LV_GRID_TEMPLATE_LAST);
}

void JZGridLayout::doLayout()
{
    lv_obj_t* obj = m_target;
    if (!obj) return;

    buildColDesc(m_colDescCache);
    buildRowDesc(m_rowDescCache);

    lv_obj_set_layout(obj, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(obj, m_colDescCache.data(), m_rowDescCache.data());

    if (m_marginLeft >= 0)   lv_obj_set_style_pad_left(obj, m_marginLeft, 0);
    if (m_marginTop >= 0)    lv_obj_set_style_pad_top(obj, m_marginTop, 0);
    if (m_marginRight >= 0)  lv_obj_set_style_pad_right(obj, m_marginRight, 0);
    if (m_marginBottom >= 0) lv_obj_set_style_pad_bottom(obj, m_marginBottom, 0);

    int h = horizontalSpacing();
    int v = verticalSpacing();
    if (h > 0 || v > 0) {
        if (h == v) {
            lv_obj_set_style_pad_gap(obj, h, 0);
        } else {
            if (h > 0) lv_obj_set_style_pad_column(obj, h, 0);
            if (v > 0) lv_obj_set_style_pad_row(obj, v, 0);
        }
    } else if (m_gap > 0) {
        lv_obj_set_style_pad_gap(obj, m_gap, 0);
    }

    for (const auto& it : m_items) {
        if (!it.widget) continue;
        lv_obj_t* child = it.widget->handle();
        if (!child) continue;
        lv_obj_set_grid_cell(child, it.colAlign, (uint8_t)it.col, (uint8_t)it.colSpan,
                                   it.rowAlign, (uint8_t)it.row, (uint8_t)it.rowSpan);
    }
}
