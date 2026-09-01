#include "JZLayout.h"
#include "JZWidget.h"
#include <algorithm>
#include <climits>

namespace {

// 在单条轴上, 依据 min/max/weight 把可用空间分配给各单元。
// hint/min/max/weight 长度均为 n; weight>0 表示该项参与增长(也会参与收缩)。
std::vector<int> distributeAxis(int avail,
                                const std::vector<int>& hint,
                                const std::vector<int>& mn,
                                const std::vector<int>& mx,
                                const std::vector<int>& weight)
{
    std::vector<int> size(hint.size(), 0);
    int usedFixed = 0;   // weight==0 项占用
    int usedFlex = 0;    // weight>0 项的初始 hint 占用 (增长时以此为基准)
    int totalW = 0;
    for (size_t i = 0; i < hint.size(); ++i) {
        size[i] = std::max(mn[i], std::min(hint[i], mx[i]));
        if (weight[i] > 0) { totalW += weight[i]; usedFlex += size[i]; }
        else               usedFixed += size[i];
    }

    int remaining = avail - usedFixed - usedFlex;
    if (remaining > 0 && totalW > 0) {
        int guard = 0;
        while (remaining > 0 && guard < 4096) {
            int wsum = 0;
            for (size_t i = 0; i < size.size(); ++i)
                if (weight[i] > 0 && size[i] < mx[i]) wsum += weight[i];
            if (wsum == 0) break;
            int gave = 0;
            for (size_t i = 0; i < size.size(); ++i) {
                if (!(weight[i] > 0 && size[i] < mx[i])) continue;
                int give = remaining * weight[i] / wsum;
                if (give <= 0) give = 1;
                int room = mx[i] - size[i];
                int d = std::min(give, room);
                if (d > 0) { size[i] += d; gave += d; }
            }
            if (gave == 0) break;
            remaining -= gave;
            ++guard;
        }
    } else if (remaining < 0) {
        remaining = -remaining;
        int guard = 0;
        while (remaining > 0 && guard < 4096) {
            int wsum = 0;
            for (size_t i = 0; i < size.size(); ++i)
                if (size[i] > mn[i]) wsum += std::max(weight[i], 1);
            if (wsum == 0) break;
            int took = 0;
            for (size_t i = 0; i < size.size(); ++i) {
                if (!(size[i] > mn[i])) continue;
                int w = std::max(weight[i], 1);
                int take = remaining * w / wsum;
                if (take <= 0) take = 1;
                int room = size[i] - mn[i];
                int d = std::min(take, room);
                if (d > 0) { size[i] -= d; took += d; }
            }
            if (took == 0) break;
            remaining -= took;
            ++guard;
        }
    }
    return size;
}

// 倾向占据更多空间的策略 (参考 Qt 的 Expanding/MinimumExpanding/Ignored)
inline bool policyExpands(JZWidget::SizePolicy p)
{
    return p == JZWidget::SizePolicy::Expanding
        || p == JZWidget::SizePolicy::MinimumExpanding
        || p == JZWidget::SizePolicy::Ignored;
}

} // namespace

// ==================== JZLayout ====================

void JZLayout::apply(JZWidget* container)
{
    m_container = container;
    relayout();
}

void JZLayout::relayout()
{
    if (m_container) {
        m_container->updateLayout();   // 重排自身子控件(含本次变更)
        m_container->updateGeometry(); // 向上级联: 父布局按新 sizeHint 重排
    }
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
    relayout();
}

void JZBoxLayout::addSpacing(int size)
{
    Item it;
    it.type = SpacerItem;
    it.spaceSize = size < 0 ? 0 : size;
    m_items.push_back(it);
    relayout();
}

void JZBoxLayout::addStretch(int stretch)
{
    Item it;
    it.type = StretchItem;
    it.stretch = stretch <= 0 ? 1 : stretch;
    m_items.push_back(it);
    relayout();
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
    relayout();
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
    relayout();
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
    relayout();
}

void JZBoxLayout::removeAt(int index)
{}

void JZBoxLayout::clear()
{
    clearSpacers();
    m_items.clear();
    relayout();
}

void JZBoxLayout::clearSpacers()
{}

void JZBoxLayout::updateLayout()
{
    if (!m_container)
        return;

    int cw = m_container->width();
    int ch = m_container->height();
    int innerW = std::max(0, cw - m_marginLeft - m_marginRight);
    int innerH = std::max(0, ch - m_marginTop - m_marginBottom);
    bool horiz = (flexFlow() == 0);

    int n = (int)m_items.size();
    if (n == 0)
        return;

    int gapTotal = m_gap * (n - 1);
    int mainAvail = std::max(0, (horiz ? innerW : innerH) - gapTotal);
    int crossAvail = (horiz ? innerH : innerW);
    int crossOrigin = (horiz ? m_marginTop : m_marginLeft);

    // 主轴上每个 item 的 hint/min/max/weight
    std::vector<int> mainHint(n, 0), mainMin(n, 0), mainMax(n, INT_MAX), mainWeight(n, 0);
    for (int i = 0; i < n; ++i) {
        const auto& it = m_items[i];
        if (it.type == WidgetItem && it.widget) {
            JZSize hs = it.widget->sizeHint();
            JZWidget::SizePolicy mp = horiz ? it.widget->horizontalPolicy() : it.widget->verticalPolicy();
            mainHint[i] = horiz ? hs.width() : hs.height();
            mainMin[i]  = horiz ? it.widget->minimumWidth()  : it.widget->minimumHeight();
            mainMax[i]  = horiz ? it.widget->maximumWidth()  : it.widget->maximumHeight();
            if (it.stretch > 0)      mainWeight[i] = it.stretch;
            else if (policyExpands(mp)) mainWeight[i] = 1;
            else                     mainWeight[i] = 0;
        } else if (it.type == SpacerItem) {
            int s = std::max(0, it.spaceSize);
            mainHint[i] = s; mainMin[i] = s; mainMax[i] = s; mainWeight[i] = 0;
        } else { // StretchItem
            mainHint[i] = 0; mainMin[i] = 0; mainMax[i] = INT_MAX;
            mainWeight[i] = it.stretch > 0 ? it.stretch : 1;
        }
    }

    std::vector<int> mainSize = distributeAxis(mainAvail, mainHint, mainMin, mainMax, mainWeight);

    // 沿主轴依次摆放, 交叉轴按策略居中/填满
    int cursor = horiz ? m_marginLeft : m_marginTop;
    for (int i = 0; i < n; ++i) {
        const auto& it = m_items[i];
        if (it.type == WidgetItem && it.widget) {
            JZSize hs = it.widget->sizeHint();
            JZWidget::SizePolicy cp = horiz ? it.widget->verticalPolicy() : it.widget->horizontalPolicy();
            int hintCross = horiz ? hs.height() : hs.width();
            int cmn = horiz ? it.widget->minimumHeight() : it.widget->minimumWidth();
            int cmx = horiz ? it.widget->maximumHeight() : it.widget->maximumWidth();
            // 交叉轴: 除 Fixed 外一律填满 (HBox 子项填满高度 / VBox 子项填满宽度)
            bool fillCross = it.stretch > 0 || cp != JZWidget::SizePolicy::Fixed;
            int desired = fillCross ? crossAvail : hintCross;
            // clamp 到交叉可用空间, 防止 hint 大于容器时溢出
            int crossSize = std::max(cmn, std::min(desired, std::min(cmx, crossAvail)));
            int crossPos = crossOrigin + (crossAvail - crossSize) / 2;
            int x = horiz ? cursor     : crossPos;
            int y = horiz ? crossPos   : cursor;
            int w = horiz ? mainSize[i] : crossSize;
            int h = horiz ? crossSize   : mainSize[i];
            it.widget->setGeometry(x, y, w, h);
        }
        cursor += mainSize[i] + m_gap;
    }
}

JZSize JZBoxLayout::sizeHint() const
{
    int n = (int)m_items.size();
    if (n == 0 || !m_container)
        return JZSize(m_marginLeft + m_marginRight, m_marginTop + m_marginBottom);

    bool horiz = (flexFlow() == 0);
    int main = 0;
    int cross = 0;
    for (int i = 0; i < n; ++i) {
        const auto& it = m_items[i];
        if (it.type == WidgetItem && it.widget) {
            JZSize hs = it.widget->sizeHint();
            if (horiz) { main += hs.width();  cross = std::max(cross, hs.height()); }
            else       { main += hs.height(); cross = std::max(cross, hs.width()); }
        } else if (it.type == SpacerItem) {
            main += std::max(0, it.spaceSize);
        }
        // StretchItem 无固有尺寸, 不计入 hint
    }
    main += m_gap * (n - 1);
    int w = horiz ? (m_marginLeft + m_marginRight + main)
                  : (m_marginLeft + m_marginRight + cross);
    int h = horiz ? (m_marginTop + m_marginBottom + cross)
                  : (m_marginTop + m_marginBottom + main);
    return JZSize(w, h);
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
    relayout();
}

void JZGridLayout::setRowStretch(int row, int stretch)
{
    if (row < 0) return;
    if ((size_t)row >= m_rowSpecs.size()) m_rowSpecs.resize(row + 1);
    m_rowSpecs[row].stretch = stretch;
    relayout();
}

void JZGridLayout::setColumnStretch(int col, int stretch)
{
    if (col < 0) return;
    if ((size_t)col >= m_colSpecs.size()) m_colSpecs.resize(col + 1);
    m_colSpecs[col].stretch = stretch;
    relayout();
}

void JZGridLayout::setRowMinimumHeight(int row, int minHeight)
{
    if (row < 0) return;
    if ((size_t)row >= m_rowSpecs.size()) m_rowSpecs.resize(row + 1);
    m_rowSpecs[row].minSize = minHeight;
    relayout();
}

void JZGridLayout::setColumnMinimumWidth(int col, int minWidth)
{
    if (col < 0) return;
    if ((size_t)col >= m_colSpecs.size()) m_colSpecs.resize(col + 1);
    m_colSpecs[col].minSize = minWidth;
    relayout();
}

void JZGridLayout::setHorizontalSpacing(int spacing)
{
    m_hGap = spacing;
    relayout();
}

void JZGridLayout::setVerticalSpacing(int spacing)
{
    m_vGap = spacing;
    relayout();
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

void JZGridLayout::updateLayout()
{
    if (!m_container)
        return;

    int cw = m_container->width();
    int ch = m_container->height();
    int innerW = std::max(0, cw - m_marginLeft - m_marginRight);
    int innerH = std::max(0, ch - m_marginTop - m_marginBottom);
    int cols = columnCount();
    int rows = rowCount();
    if (cols <= 0 || rows <= 0)
        return;

    int hGap = horizontalSpacing();
    int vGap = verticalSpacing();

    std::vector<int> colHint(cols, 0), colMin(cols, 0), colMax(cols, INT_MAX), colW(cols, 0);
    std::vector<int> rowHint(rows, 0), rowMin(rows, 0), rowMax(rows, INT_MAX), rowW(rows, 0);
    std::vector<int> colWeight(cols, 0), rowWeight(rows, 0);

    // 每个单元格的 hint/min/max, 以及来自控件扩展策略的权重
    for (const auto& it : m_items) {
        if (!it.widget) continue;
        JZSize hs = it.widget->sizeHint();
        if (it.colSpan == 1) {
            colHint[it.col] = std::max(colHint[it.col], hs.width());
            colMin[it.col]  = std::max(colMin[it.col], it.widget->minimumWidth());
            if (it.widget->maximumWidth() < INT_MAX)
                colMax[it.col] = std::min(colMax[it.col], it.widget->maximumWidth());
            if (policyExpands(it.widget->horizontalPolicy()))
                colWeight[it.col] = std::max(colWeight[it.col], 1);
        }
        if (it.rowSpan == 1) {
            rowHint[it.row] = std::max(rowHint[it.row], hs.height());
            rowMin[it.row]  = std::max(rowMin[it.row], it.widget->minimumHeight());
            if (it.widget->maximumHeight() < INT_MAX)
                rowMax[it.row] = std::min(rowMax[it.row], it.widget->maximumHeight());
            if (policyExpands(it.widget->verticalPolicy()))
                rowWeight[it.row] = std::max(rowWeight[it.row], 1);
        }
    }

    // 行列 stretch / 最小尺寸规格
    for (int c = 0; c < cols; ++c) {
        int s = (c < (int)m_colSpecs.size()) ? m_colSpecs[c].stretch : 0;
        if (s > 0) colWeight[c] = std::max(colWeight[c], s);
        if (c < (int)m_colSpecs.size() && m_colSpecs[c].minSize >= 0)
            colMin[c] = std::max(colMin[c], m_colSpecs[c].minSize);
    }
    for (int r = 0; r < rows; ++r) {
        int s = (r < (int)m_rowSpecs.size()) ? m_rowSpecs[r].stretch : 0;
        if (s > 0) rowWeight[r] = std::max(rowWeight[r], s);
        if (r < (int)m_rowSpecs.size() && m_rowSpecs[r].minSize >= 0)
            rowMin[r] = std::max(rowMin[r], m_rowSpecs[r].minSize);
    }

    colW = distributeAxis(innerW - hGap * (cols - 1), colHint, colMin, colMax, colWeight);
    rowW = distributeAxis(innerH - vGap * (rows - 1), rowHint, rowMin, rowMax, rowWeight);

    // 累计行列起点
    std::vector<int> colX(cols, 0), rowY(rows, 0);
    int cx = m_marginLeft;
    for (int c = 0; c < cols; ++c) { colX[c] = cx; cx += colW[c] + hGap; }
    int ry = m_marginTop;
    for (int r = 0; r < rows; ++r) { rowY[r] = ry; ry += rowW[r] + vGap; }

    // 放置每个控件 (处理合并单元格), 单元格内居中
    for (const auto& it : m_items) {
        if (!it.widget) continue;
        int cellX = colX[it.col];
        int cellY = rowY[it.row];
        int cellW = 0, cellH = 0;
        for (int c = it.col; c < it.col + it.colSpan; ++c) cellW += colW[c];
        for (int r = it.row; r < it.row + it.rowSpan; ++r) cellH += rowW[r];
        if (it.colSpan > 1) cellW += hGap * (it.colSpan - 1);
        if (it.rowSpan > 1) cellH += vGap * (it.rowSpan - 1);

        JZWidget::SizePolicy hp = it.widget->horizontalPolicy();
        JZWidget::SizePolicy vp = it.widget->verticalPolicy();
        bool hFill = policyExpands(hp) || hp == JZWidget::SizePolicy::Preferred;
        bool vFill = policyExpands(vp) || vp == JZWidget::SizePolicy::Preferred;

        int w = hFill ? cellW : it.widget->sizeHint().width();
        int h = vFill ? cellH : it.widget->sizeHint().height();
        w = std::max(it.widget->minimumWidth(),  std::min(w, it.widget->maximumWidth()));
        h = std::max(it.widget->minimumHeight(), std::min(h, it.widget->maximumHeight()));
        int gx = cellX + (cellW - w) / 2;
        int gy = cellY + (cellH - h) / 2;
        it.widget->setGeometry(gx, gy, w, h);
    }
}

JZSize JZGridLayout::sizeHint() const
{
    int cols = columnCount();
    int rows = rowCount();
    if (cols <= 0 || rows <= 0)
        return JZSize(m_marginLeft + m_marginRight, m_marginTop + m_marginBottom);

    std::vector<int> colHint(cols, 0), rowHint(rows, 0);
    for (const auto& it : m_items) {
        if (!it.widget) continue;
        JZSize hs = it.widget->sizeHint();
        if (it.colSpan == 1) colHint[it.col] = std::max(colHint[it.col], hs.width());
        if (it.rowSpan == 1) rowHint[it.row] = std::max(rowHint[it.row], hs.height());
    }
    int w = m_marginLeft + m_marginRight;
    int h = m_marginTop + m_marginBottom;
    for (int c = 0; c < cols; ++c) w += colHint[c];
    for (int r = 0; r < rows; ++r) h += rowHint[r];
    int hGap = horizontalSpacing();
    int vGap = verticalSpacing();
    w += hGap * (cols - 1);
    h += vGap * (rows - 1);
    return JZSize(w, h);
}
