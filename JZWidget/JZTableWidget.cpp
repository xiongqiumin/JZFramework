#include "JZTableWidget.h"

#include "JZColor.h"
#include "JZBrush.h"
#include "JZPen.h"
#include "JZFont.h"

JZ_OBJECT_IMPL(JZTableWidget, JZScrollArea)

// ============================================================================
// JZTableWidget (owner-draw 虚拟表格, 继承 JZScrollArea)
//   - 自身即滚动区: 双向滚动, 内容由 m_content 撑开
//   - paintEvent 先画可见表体, 再在顶部叠加冻结表头(水平随滚动同步, 垂直固定)
// ============================================================================
JZTableWidget::JZTableWidget(JZWidget* parent)
    : JZScrollArea(parent)
    , currentRowChanged(this)
    , currentColumnChanged(this)
{}

JZTableWidget::~JZTableWidget()
{
}

void JZTableWidget::invalidateAll()
{
    update();
}

void JZTableWidget::setColumnCount(int cols)
{
    if (cols < 0) cols = 0;
    m_cols = cols;
    for (auto& row : m_grid)
        row.resize(cols);
    m_colWidths.assign(cols, 80);
    m_colOffsets.assign(cols, 0);
    updateContentSize();
    invalidateAll();
}

int JZTableWidget::columnCount() const
{
    return m_cols;
}

void JZTableWidget::setRowCount(int rows)
{
    if (rows < 0) rows = 0;
    m_grid.resize(rows, std::vector<JZTableWidgetItem>(m_cols));
    m_curRow = -1;
    m_curCol = -1;
    updateContentSize();
    invalidateAll();
}

int JZTableWidget::rowCount() const
{
    return (int)m_grid.size();
}

void JZTableWidget::setItem(int row, int col, const JZTableWidgetItem& item)
{
    if (row < 0 || row >= (int)m_grid.size()) return;
    if (col < 0 || col >= m_cols) return;
    JZTableWidgetItem it = item;
    it.widget = this;
    m_grid[row][col] = std::move(it);
    invalidateAll();
}

const JZTableWidgetItem& JZTableWidget::item(int row, int col) const
{
    static JZTableWidgetItem s_empty;
    if (row < 0 || row >= (int)m_grid.size()) return s_empty;
    if (col < 0 || col >= m_cols) return s_empty;
    return m_grid[row][col];
}

void JZTableWidget::setCellValue(int row, int col, const std::string& text)
{
    JZTableWidgetItem it;
    it.widget = this;
    it.text = text;
    setItem(row, col, it);
}

std::string JZTableWidget::cellValue(int row, int col) const
{
    if (row < 0 || row >= (int)m_grid.size() || col < 0 || col >= m_cols)
        return {};
    return m_grid[row][col].text;
}

void JZTableWidget::setColumnWidth(int col, int w)
{
    if (col < 0 || col >= m_cols || w <= 0) return;
    m_colWidths[col] = w;
    updateContentSize();
    invalidateAll();
}

void JZTableWidget::setHorizontalHeaderLabels(const std::vector<std::string>& labels)
{
    m_headerLabels = labels;
    update();
}

void JZTableWidget::setRowHeight(int h)
{
    if (h <= 0) return;
    m_rowHeight = h;
    updateContentSize();
    invalidateAll();
}

int JZTableWidget::rowHeight() const
{
    return m_rowHeight;
}

int JZTableWidget::headerHeight() const
{
    return m_rowHeight;
}

void JZTableWidget::setItemPaint(PaintFn fn)
{
    m_paintFn = std::move(fn);
    invalidateAll();
}

void JZTableWidget::setCurrentCell(int row, int col)
{
    if (row < 0 || row >= (int)m_grid.size() || col < 0 || col >= m_cols)
        return;
    bool rowChanged = (row != m_curRow);
    bool colChanged = (col != m_curCol);
    if (!rowChanged && !colChanged) return;
    m_curRow = row;
    m_curCol = col;
    invalidateAll();
    if (rowChanged) currentRowChanged.emit(row);
    if (colChanged) currentColumnChanged.emit(col);
}

int JZTableWidget::currentRow() const
{
    return m_curRow;
}

int JZTableWidget::currentColumn() const
{
    return m_curCol;
}

// ----------------------------------------------------------------------------
// 表头 / 列几何
// ----------------------------------------------------------------------------
int JZTableWidget::colX(int col) const
{
    if (col < 0 || col >= (int)m_colOffsets.size()) return 0;
    return m_colOffsets[col];
}

int JZTableWidget::colFromX(int x) const
{
    for (int c = 0; c < m_cols; c++) {
        if (x >= m_colOffsets[c] && x < m_colOffsets[c] + m_colWidths[c])
            return c;
    }
    return -1;
}

int JZTableWidget::totalWidth() const
{
    if (m_cols == 0) return 0;
    return m_colOffsets.back() + m_colWidths.back();
}

void JZTableWidget::updateContentSize()
{
    if (m_cols != (int)m_colWidths.size())
        m_colWidths.assign(m_cols, 80);
    if (m_cols != (int)m_colOffsets.size())
        m_colOffsets.assign(m_cols, 0);

    int x = 0;
    for (int i = 0; i < m_cols; i++) {
        m_colOffsets[i] = x;
        x += m_colWidths[i];
    }
    m_totalW = x;

    int hh = headerHeight();
    setContentSize(m_totalW, hh + m_rowHeight * (int)m_grid.size());
}

// 内置默认绘图: 背景(选中/普通) + 图标 + 文本 + 列/行分隔线
void JZTableWidget::defaultPaint(JZPainter& p, const JZRect& rect, JZTableWidgetItem* item, int row, int col)
{
    bool sel = (row == m_curRow && col == m_curCol);
    if (sel)
        p.fillRect(rect.x(), rect.y(), rect.width(), rect.height(), JZColor(0x33, 0x66, 0xcc));
    // 未选中: 不填充, 透出白底, 黑字

    int iconSize = 16;
    int textX = rect.x() + 4;
    if (item->icon.dsc()) {
        int ix = rect.x() + 4;
        int iy = rect.y() + (rect.height() - iconSize) / 2;
        p.drawImage(item->icon, ix, iy, iconSize, iconSize);
        textX = ix + iconSize + 4;
    }

    JZBrush brush; brush.setColor(sel ? JZColor::White() : JZColor::Black()); p.setBrush(brush);
    JZFont font(13); p.setFont(font);
    int ty = rect.y() + (rect.height() - 14) / 2;
    p.drawText(textX, ty, item->text.c_str());

    // 列/行分隔线
    JZPen pen(JZColor(0x44, 0x44, 0x44), 1); p.setPen(pen);
    p.drawLine(rect.x(), rect.y(), rect.x() + rect.width(), rect.y());
    p.drawLine(rect.x() + rect.width() - 1, rect.y(),
               rect.x() + rect.width() - 1, rect.y() + rect.height());
}

// 冻结表头(覆盖在视口顶部, 水平随滚动同步, 垂直固定)
void JZTableWidget::paintHeader(JZPainter& p)
{
    int sx = scrollX();
    int viewW = viewportWidth();
    int hh = headerHeight();

    p.fillRect(0, 0, viewW, hh, JZColor(0xe8, 0xe8, 0xe8));

    JZBrush brush; brush.setColor(JZColor::Black()); p.setBrush(brush);
    JZFont font(13); p.setFont(font);

    for (int c = 0; c < m_cols; c++) {
        int cx = colX(c) - sx;
        int cw = m_colWidths[c];
        if (cx + cw < 0 || cx > viewW) continue;

        std::string txt = (c < (int)m_headerLabels.size()) ? m_headerLabels[c] : "";
        p.drawText(cx + 4, (hh - 14) / 2 + 2, txt.c_str());

        JZPen pen(JZColor(0xbb, 0xbb, 0xbb), 1); p.setPen(pen);
        p.drawLine(cx + cw, 1, cx + cw, hh - 1);
    }

    // 表头底边分隔线
    JZPen hpen(JZColor(0x99, 0x99, 0x99), 1); p.setPen(hpen);
    p.drawLine(0, hh - 1, viewW, hh - 1);
}

// ----------------------------------------------------------------------------
// 绘制(表体 + 冻结表头)
// ----------------------------------------------------------------------------
void JZTableWidget::paintEvent(JZPainterEvent* event)
{
    JZPainter& p = *event->painter();

    int sx = scrollX();
    int sy = scrollY();
    int viewW = viewportWidth();
    int viewH = viewportHeight();
    int cols = m_cols;
    int rows = (int)m_grid.size();
    int rh = m_rowHeight;
    int hh = headerHeight();

    // ---- 可见表体 ----
    if (cols > 0 && rows > 0) {
        int firstCol = 0;
        while (firstCol < cols && m_colOffsets[firstCol] + m_colWidths[firstCol] <= sx)
            firstCol++;
        int lastCol = firstCol;
        while (lastCol < cols && m_colOffsets[lastCol] < sx + viewW)
            lastCol++;
        if (firstCol > 0) firstCol--;
        if (lastCol < cols - 1) lastCol++;
        if (lastCol >= cols) lastCol = cols - 1;

        int firstRow = (sy - hh) / rh;
        if (firstRow < 0) firstRow = 0;
        int lastRow = (sy - hh + viewH) / rh + 1;
        if (lastRow > rows) lastRow = rows;

        for (int r = firstRow; r < lastRow; r++) {
            for (int c = firstCol; c <= lastCol; c++) {
                int cx = m_colOffsets[c] - sx;
                int cy = hh + r * rh - sy;
                if (cx + m_colWidths[c] < 0 || cx > viewW) continue;
                if (cy + rh < 0 || cy > viewH) continue;
                JZRect rect(cx, cy, m_colWidths[c], rh);
                if (m_paintFn)
                    m_paintFn(p, rect, &m_grid[r][c]);
                else
                    defaultPaint(p, rect, &m_grid[r][c], r, c);
            }
        }
    }

    // ---- 冻结表头(覆盖在顶部, 屏蔽表体溢出) ----
    if (m_cols > 0)
        paintHeader(p);
}

// ----------------------------------------------------------------------------
// 事件
// ----------------------------------------------------------------------------
void JZTableWidget::showEvent(JZShowEvent* e)
{
    JZScrollArea::showEvent(e);
    invalidateAll();
}

void JZTableWidget::resizeEvent(JZResizeEvent* e)
{
    JZScrollArea::resizeEvent(e);
    invalidateAll();
}
