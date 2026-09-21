#include "JZTableWidget.h"

#include "JZColor.h"
#include "JZBrush.h"
#include "JZPen.h"
#include "JZFont.h"
#include "JZFontMetrics.h"

JZ_OBJECT_IMPL(JZTableWidget, JZAbstractViewWidget)

// ============================================================================
// JZTableWidget (owner-draw 虚拟表格, 继承 JZAbstractViewWidget)
//   - 单元格数据用 unique_ptr 持有(指针语义, 地址稳定; 接口形态对齐 Qt QTableWidget)
//   - 滚动条: 集成 JZScrollArea 的真实 JZScrollBar (内容双向滚动)
//   - paintEvent 先画可见表体, 再在顶部叠加冻结表头(水平随滚动同步, 垂直固定)
// ============================================================================
JZTableWidget::JZTableWidget(JZWidget* parent)
    : JZAbstractViewWidget(parent)
    , currentRowChanged(this)
    , currentColumnChanged(this)
    , sigDataChanged(this)
{}

JZTableWidget::~JZTableWidget()
{
}

void JZTableWidget::invalidateAll()
{
    update();
}

// ---- JZTableWidgetItem: 改值并通知所属表格重绘 (在 JZTableWidget 之后定义, 此处类型已完整) ----

void JZTableWidgetItem::setText(const std::string& t)
{
    m_text = t;
    if (m_table)
    {
        m_table->update();
        m_table->notifyDataChanged();
    }
}

void JZTableWidgetItem::setIcon(const JZImage& i)
{
    m_icon = i;
    if (m_table)
    {
        m_table->update();
        m_table->notifyDataChanged();
    }
}

void JZTableWidgetItem::setData(const JZVariant& d)
{
    m_data = d;
    if (m_table)
    {
        m_table->update();   // 自定义 itemPaint 可能依据 data 绘制, 故同样重绘
        m_table->notifyDataChanged();
    }
}

// ----------------------------------------------------------------------------
// 内部: 落位 / 摘除
// ----------------------------------------------------------------------------

// 摘除(转移所有权, 不删除); 越界或空格返回 nullptr
std::unique_ptr<JZTableWidgetItem> JZTableWidget::detachAt(int row, int col)
{
    if (row < 0 || row >= (int)m_grid.size()) return nullptr;
    if (col < 0 || col >= (int)m_grid[row].size()) return nullptr;

    std::unique_ptr<JZTableWidgetItem> item = std::move(m_grid[row][col]);
    if (item)
    {
        item->m_table = nullptr;
        item->m_row = -1;
        item->m_col = -1;
    }
    return item;
}

// 落位: 顶掉该格原有 item(自动 delete); item 为 nullptr 表示清空该格
void JZTableWidget::placeItem(int row, int col, std::unique_ptr<JZTableWidgetItem> item)
{
    if (row < 0 || row >= (int)m_grid.size()) return;
    if (col < 0 || col >= (int)m_grid[row].size()) return;

    // 同一个 item 之前挂在别处: 先把旧格子的持有权放开(所有权已在 item 手里, 不能重复释放)
    if (item && item->m_table == this && (item->m_row != row || item->m_col != col))
    {
        int oldRow = item->m_row;
        int oldCol = item->m_col;
        if (oldRow >= 0 && oldRow < (int)m_grid.size() &&
            oldCol >= 0 && oldCol < (int)m_grid[oldRow].size() &&
            m_grid[oldRow][oldCol].get() == item.get())
            m_grid[oldRow][oldCol].release();
    }

    m_grid[row][col] = std::move(item);

    if (m_grid[row][col])
    {
        m_grid[row][col]->m_table = this;
        m_grid[row][col]->m_row = row;
        m_grid[row][col]->m_col = col;
    }
}

// 行列变动后刷新各 item 记录的行列号
void JZTableWidget::reindexItems()
{
    for (int r = 0; r < (int)m_grid.size(); r++)
    {
        for (int c = 0; c < (int)m_grid[r].size(); c++)
        {
            if (!m_grid[r][c])
                continue;
            m_grid[r][c]->m_row = r;
            m_grid[r][c]->m_col = c;
        }
    }
}

// ----------------------------------------------------------------------------
// 数据模型
// ----------------------------------------------------------------------------

void JZTableWidget::setItem(int row, int col, JZTableWidgetItem* item)
{
    if (row < 0 || row >= (int)m_grid.size()) return;
    if (col < 0 || col >= m_cols) return;

    placeItem(row, col, std::unique_ptr<JZTableWidgetItem>(item));
    invalidateAll();
    notifyDataChanged();
}

JZTableWidgetItem* JZTableWidget::item(int row, int col) const
{
    if (row < 0 || row >= (int)m_grid.size()) return nullptr;
    if (col < 0 || col >= (int)m_grid[row].size()) return nullptr;
    return m_grid[row][col].get();
}

JZTableWidgetItem* JZTableWidget::takeItem(int row, int col)
{
    std::unique_ptr<JZTableWidgetItem> item = detachAt(row, col);
    if (!item)
        return nullptr;

    invalidateAll();
    notifyDataChanged();
    return item.release();      // 所有权交回调用方
}

int JZTableWidget::row(const JZTableWidgetItem* item) const
{
    if (!item) return -1;
    for (int r = 0; r < (int)m_grid.size(); r++)
    {
        for (int c = 0; c < (int)m_grid[r].size(); c++)
        {
            if (m_grid[r][c].get() == item)
                return r;
        }
    }
    return -1;
}

int JZTableWidget::column(const JZTableWidgetItem* item) const
{
    if (!item) return -1;
    for (int r = 0; r < (int)m_grid.size(); r++)
    {
        for (int c = 0; c < (int)m_grid[r].size(); c++)
        {
            if (m_grid[r][c].get() == item)
                return c;
        }
    }
    return -1;
}

void JZTableWidget::setColumnCount(int cols)
{
    if (cols < 0) cols = 0;

    m_cols = cols;
    for (auto& row : m_grid)
        row.resize(cols);           // 收缩时被裁掉的 item 随 unique_ptr 一起释放

    // 已有列的宽度保留, 新增列给默认宽度
    m_colWidths.resize(cols, 80);
    m_colOffsets.resize(cols, 0);

    if (m_curCol >= cols)
    {
        m_curRow = -1;
        m_curCol = -1;
    }

    updateContentSize();
    invalidateAll();
    notifyDataChanged();
}

int JZTableWidget::columnCount() const
{
    return m_cols;
}

void JZTableWidget::setRowCount(int rows)
{
    if (rows < 0) rows = 0;

    int old = (int)m_grid.size();
    m_grid.resize(rows);                        // 收缩时被删的行连同 item 一起释放
    for (int r = old; r < rows; r++)
        m_grid[r].resize(m_cols);               // 新增行: m_cols 个空单元格(unique_ptr 默认 nullptr)

    if (m_curRow >= rows)
    {
        m_curRow = -1;
        m_curCol = -1;
    }

    updateContentSize();
    invalidateAll();
    notifyDataChanged();
}

int JZTableWidget::rowCount() const
{
    return (int)m_grid.size();
}

void JZTableWidget::insertRow(int row)
{
    if (row < 0 || row > (int)m_grid.size()) return;

    m_grid.insert(m_grid.begin() + row,
                  std::vector<std::unique_ptr<JZTableWidgetItem>>(m_cols));

    if (m_curRow >= row)
        m_curRow++;                             // 选中行顺延, 仍指向同一行数据

    reindexItems();
    updateContentSize();
    invalidateAll();
    notifyDataChanged();
}

void JZTableWidget::removeRow(int row)
{
    if (row < 0 || row >= (int)m_grid.size()) return;

    m_grid.erase(m_grid.begin() + row);         // 该行 item 随之释放

    if (m_curRow == row)
    {
        m_curRow = -1;
        m_curCol = -1;
    }
    else if (m_curRow > row)
    {
        m_curRow--;
    }

    reindexItems();
    updateContentSize();
    invalidateAll();
    notifyDataChanged();
}

void JZTableWidget::insertColumn(int col)
{
    if (col < 0 || col > m_cols) return;

    for (auto& row : m_grid)
        row.insert(row.begin() + col, std::unique_ptr<JZTableWidgetItem>());

    m_cols++;
    m_colWidths.insert(m_colWidths.begin() + col, 80);
    m_colOffsets.insert(m_colOffsets.begin() + col, 0);

    if (m_curCol >= col)
        m_curCol++;

    reindexItems();
    updateContentSize();
    invalidateAll();
    notifyDataChanged();
}

void JZTableWidget::removeColumn(int col)
{
    if (col < 0 || col >= m_cols) return;

    for (auto& row : m_grid)
    {
        if (col < (int)row.size())
            row.erase(row.begin() + col);       // 该列 item 随之释放
    }

    m_cols--;
    if (col < (int)m_colWidths.size())
        m_colWidths.erase(m_colWidths.begin() + col);
    if (col < (int)m_colOffsets.size())
        m_colOffsets.erase(m_colOffsets.begin() + col);

    if (m_curCol == col)
    {
        m_curRow = -1;
        m_curCol = -1;
    }
    else if (m_curCol > col)
    {
        m_curCol--;
    }

    reindexItems();
    updateContentSize();
    invalidateAll();
    notifyDataChanged();
}

void JZTableWidget::clearContents()
{
    for (auto& row : m_grid)
    {
        for (auto& cell : row)
            cell.reset();
    }

    m_curRow = -1;
    m_curCol = -1;
    invalidateAll();
    notifyDataChanged();
}

void JZTableWidget::clear()
{
    clearContents();
    m_headerLabels.clear();
    invalidateAll();
    notifyDataChanged();   // 表头也算数据(headerLabels() 读得到)
}

void JZTableWidget::notifyDataChanged()
{
    sigDataChanged.emit();
}

void JZTableWidget::setItemText(int row, int col, const std::string& text)
{
    if (row < 0 || row >= (int)m_grid.size()) return;
    if (col < 0 || col >= m_cols) return;

    JZTableWidgetItem* it = m_grid[row][col].get();
    if (it)
    {
        it->setText(text);      // item 自己通知重绘
        return;
    }

    placeItem(row, col, std::unique_ptr<JZTableWidgetItem>(new JZTableWidgetItem(text)));
    invalidateAll();
    notifyDataChanged();
}

std::string JZTableWidget::itemText(int row, int col) const
{
    JZTableWidgetItem* it = item(row, col);
    return it ? it->text() : std::string();
}

void JZTableWidget::setCellValue(int row, int col, const std::string& text)
{
    setItemText(row, col, text);
}

std::string JZTableWidget::cellValue(int row, int col) const
{
    return itemText(row, col);
}

// ----------------------------------------------------------------------------
// 视图
// ----------------------------------------------------------------------------

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
    notifyDataChanged();
}

// 表头文本快照(与 setHorizontalHeaderLabels 对称; JS 侧 setHeaderLabels/headerLabels 用它读回)
std::vector<std::string> JZTableWidget::headerLabels() const
{
    return m_headerLabels;
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

// ----------------------------------------------------------------------------
// 选择
// ----------------------------------------------------------------------------

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

JZTableWidgetItem* JZTableWidget::currentItem() const
{
    return item(m_curRow, m_curCol);
}

void JZTableWidget::setCurrentItem(JZTableWidgetItem* item, bool emitSignal)
{
    if (!item)
    {
        m_curRow = -1;      // 传 nullptr = 清空选择(与 Qt 一致)
        m_curCol = -1;
        invalidateAll();
        return;
    }

    int r = row(item);
    int c = column(item);
    if (r < 0 || c < 0)
        return;             // 该 item 不在本表

    if (emitSignal)
    {
        setCurrentCell(r, c);
        return;
    }

    if (r == m_curRow && c == m_curCol)
        return;
    m_curRow = r;
    m_curCol = c;
    invalidateAll();
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
        m_colWidths.resize(m_cols, 80);
    if (m_cols != (int)m_colOffsets.size())
        m_colOffsets.resize(m_cols, 0);

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

    if (item)
    {
        int iconSize = 16;
        int textX = rect.x() + 4;
        if (!item->icon().isNull()) {
            int ix = rect.x() + 4;
            int iy = rect.y() + (rect.height() - iconSize) / 2;
            p.drawImage(item->icon(), ix, iy, iconSize, iconSize);
            textX = ix + iconSize + 4;
        }

        JZBrush brush; brush.setColor(sel ? JZColor::White : JZColor::Black); p.setBrush(brush);
        JZFont font(13); p.setFont(font);
        int ty = rect.y() + JZFontMetrics::centerY(font, rect.height());
        p.drawText(textX, ty, item->text().c_str());
    }

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

    p.fillRect(0, 0, width(), hh, JZColor(0xe8, 0xe8, 0xe8));

    JZBrush brush; brush.setColor(JZColor::Black); p.setBrush(brush);
    JZFont font(13); p.setFont(font);

    for (int c = 0; c < m_cols; c++) {
        int cx = colX(c) - sx;
        int cw = m_colWidths[c];
        if (cx + cw < 0 || cx > viewW) continue;

        std::string txt = (c < (int)m_headerLabels.size()) ? m_headerLabels[c] : "";
        p.drawText(cx + 4, JZFontMetrics::centerY(font, hh), txt.c_str());

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
    JZPainter p(this);

    int sx = scrollX();
    int sy = scrollY();
    int viewW = viewportWidth();
    int viewH = viewportHeight();
    int cols = m_cols;
    int rows = (int)m_grid.size();
    int rh = m_rowHeight;
    int hh = headerHeight();

    // ---- 可见表体 ----
    // 表体白底 (表头以下区域, 铺满整宽含滚动条带宽), 未选中单元格透出白底
    if (cols > 0)
        p.fillRect(0, hh, width(), std::max(0, height() - hh), JZColor::White);

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

                JZTableWidgetItem* cell = m_grid[r][c].get();     // 空单元格为 nullptr
                if (m_paintFn)
                    m_paintFn(p, rect, cell);
                else
                    defaultPaint(p, rect, cell, r, c);
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
