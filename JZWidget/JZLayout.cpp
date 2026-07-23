#include "JZLayout.h"
#include "JZWidget.h"
#include <algorithm>

// ==================== JZLayout ====================

void JZLayout::apply(void* obj)
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
{}

void JZBoxLayout::clear()
{
    clearSpacers();
    m_items.clear();
    updateLayout();
}

void JZBoxLayout::clearSpacers()
{}

void JZBoxLayout::doLayout()
{}

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

void JZGridLayout::setRowMinimumHeight(int row, int minHeight)
{
    if (row < 0) return;
    if ((size_t)row >= m_rowSpecs.size()) m_rowSpecs.resize(row + 1);
    m_rowSpecs[row].minSize = minHeight;
    updateLayout();
}

void JZGridLayout::setColumnMinimumWidth(int col, int minWidth)
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

void JZGridLayout::buildColDesc(std::vector<int>& out) const
{
    int n = columnCount();
    out.clear();
    out.reserve(n + 1);
    for (int i = 0; i < n; ++i) {
        int v = 0;
        if (i < (int)m_colSpecs.size()) {
            const auto& s = m_colSpecs[i];
            if (s.stretch > 0) v = (int)s.stretch;
            else if (s.minSize >= 0) v = s.minSize;
        }
        out.push_back(v);
    }
    out.push_back(0);
}

void JZGridLayout::buildRowDesc(std::vector<int>& out) const
{
    int n = rowCount();
    out.clear();
    out.reserve(n + 1);
    for (int i = 0; i < n; ++i) {
        int v = 0;
        if (i < (int)m_rowSpecs.size()) {
            const auto& s = m_rowSpecs[i];
            if (s.stretch > 0) v = (int)s.stretch;
            else if (s.minSize >= 0) v = s.minSize;
        }
        out.push_back(v);
    }
    out.push_back(0);
}

void JZGridLayout::doLayout()
{}
