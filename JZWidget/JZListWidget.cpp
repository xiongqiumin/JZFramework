#include "JZListWidget.h"

#include <algorithm>

#include "JZColor.h"
#include "JZBrush.h"
#include "JZPen.h"
#include "JZFont.h"
#include "JZFontMetrics.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

JZ_OBJECT_IMPL(JZListWidget, JZAbstractViewWidget)

// ============================================================================
// JZListWidget (owner-draw 虚拟列表, 继承 JZAbstractViewWidget)
// ============================================================================
JZListWidget::JZListWidget(JZWidget* parent)
    : JZAbstractViewWidget(parent)
    , currentIndexChanged(this)
    , sigActivated(this)
{}

JZListWidget::~JZListWidget()
{
}

// ---- JZListWidgetItem: 改值并通知所属列表重绘 (在 JZListWidget 之后定义, 此处类型已完整) ----

void JZListWidgetItem::setText(const std::string& t)
{
    m_text = t;
    if (m_widget)
        m_widget->update();
}

void JZListWidgetItem::setIcon(const JZImage& i)
{
    m_icon = i;
    if (m_widget)
        m_widget->update();
}

void JZListWidgetItem::setData(const JZVariant& d)
{
    m_data = d;
    if (m_widget)
        m_widget->update();   // 自定义 itemPaint 可能依据 data 绘制, 故同样重绘
}

// ---- JZListWidget: 数据模型 ----

JZListWidgetItem* JZListWidget::insertItemAt(int index, std::unique_ptr<JZListWidgetItem> item)
{
    if (!item)
        return nullptr;

    if (index < 0 || index > (int)m_items.size())
        index = (int)m_items.size();             // 越界按追加处理

    item->m_widget = this;                       // 建立反向引用(改值 -> 通知重绘)
    JZListWidgetItem* raw = item.get();
    m_items.insert(m_items.begin() + index, std::move(item));

    if (m_current >= index)
        m_current++;                             // 选中项被挤到后一位

    updateContentSize();
    update();
    return raw;
}

void JZListWidget::appendItem(const std::string& text)
{
    insertItemAt((int)m_items.size(), std::unique_ptr<JZListWidgetItem>(new JZListWidgetItem(text)));
}

void JZListWidget::appendItem(JZListWidgetItem* item)
{
    // 接管语义(/Transfer/): item 须由 new 分配, 加入后由本列表负责 delete
    if (!item)
        return;
    insertItemAt((int)m_items.size(), std::unique_ptr<JZListWidgetItem>(item));
}

void JZListWidget::insertItem(int index, const std::string& text)
{
    insertItemAt(index, std::unique_ptr<JZListWidgetItem>(new JZListWidgetItem(text)));
}

void JZListWidget::insertItem(int index, JZListWidgetItem* item)
{
    if (!item)
        return;
    insertItemAt(index, std::unique_ptr<JZListWidgetItem>(item));
}

void JZListWidget::appendItems(const std::vector<std::string>& texts)
{
    // 批量追加: 逐个落位但不逐次重绘, 最后统一刷新
    for (const auto& t : texts)
    {
        std::unique_ptr<JZListWidgetItem> it(new JZListWidgetItem(t));
        it->m_widget = this;
        m_items.push_back(std::move(it));
    }
    updateContentSize();
    update();
}

JZListWidgetItem* JZListWidget::takeItem(int index)
{
    if (index < 0 || index >= (int)m_items.size())
        return nullptr;

    JZListWidgetItem* raw = m_items[index].release();   // 交出所有权
    raw->m_widget = nullptr;                            // 不再属于本列表
    m_items.erase(m_items.begin() + index);

    if (m_current == index)
        m_current = -1;                                 // 选中项被摘除 -> 清空选择
    else if (m_current > index)
        m_current--;

    updateContentSize();
    update();
    return raw;
}

// removeItem 已按 Qt 的做法删除: 要删除某项就 `delete list->takeItem(row)`。

void JZListWidget::clear()
{
    m_items.clear();
    m_current = -1;
    updateContentSize();
    update();
}

int JZListWidget::count() const
{
    return (int)m_items.size();
}

JZListWidgetItem* JZListWidget::item(int index) const
{
    if (index < 0 || index >= (int)m_items.size())
        return nullptr;
    return m_items[index].get();
}

int JZListWidget::row(const JZListWidgetItem* item) const
{
    if (!item)
        return -1;
    for (size_t i = 0; i < m_items.size(); i++)
        if (m_items[i].get() == item)
            return (int)i;
    return -1;
}

std::string JZListWidget::itemText(int index) const
{
    JZListWidgetItem* it = item(index);
    return it ? it->text() : std::string();
}

JZVariant JZListWidget::itemData(int index) const
{
    JZListWidgetItem* it = item(index);
    return it ? it->data() : JZVariant();
}

void JZListWidget::setViewMode(ViewMode mode)
{
    if (mode == m_mode) return;
    m_mode = mode;
    updateContentSize();
    update();
}

JZListWidget::ViewMode JZListWidget::viewMode() const
{
    return m_mode;
}

void JZListWidget::setItemHeight(int h)
{
    if (h <= 0) return;
    m_itemHeight = h;
    if (m_mode == ListMode) {
        updateContentSize();
        update();
    }
}

int JZListWidget::itemHeight() const
{
    return m_itemHeight;
}

JZSize JZListWidget::sizeHint() const
{
    // 虚拟列表自己知道该多大:
    //   宽度 = 最宽项 (文本 + 图标) + 左右留白
    //   高度 = 行数 * 行高, 至多 8 行 (再多交给滚动 —— 与下拉弹层的可见行数一致)
    const int maxVisible = 8;

    JZFont font(m_mode == ListMode ? 14 : 12);
    int w = 0;
    for (size_t i = 0; i < m_items.size(); i++)
    {
        const JZListWidgetItem& it = *m_items[i];
        int iw = JZFontMetrics::textWidth(it.text(), font);
        if (!it.icon().isNull())
            iw += 20 + 6;              // 图标 + 间距 (与 defaultPaint 一致)
        if (iw > w)
            w = iw;
    }

    int h;
    if (m_mode == ListMode)
    {
        w += 6 + 6;                    // defaultPaint 左右各留 6
        int rows = std::min(std::max(1, (int)m_items.size()), maxVisible);
        h = rows * m_itemHeight + 4;
    }
    else
    {
        w = std::max(w, m_cellW);
        int rows = std::min(std::max(1, (int)m_items.size()), maxVisible);
        h = rows * m_cellH + 4;        // 图片模式按"每项一行"给下限, 实际由视口宽度决定列数
    }

    JZSize min = minimumSizeHint();
    return JZSize(std::max(w, min.width()), std::max(h, min.height()));
}

void JZListWidget::setCellSize(int w, int h)
{
    if (w > 0) m_cellW = w;
    if (h > 0) m_cellH = h;
    if (m_mode == ImageMode) {
        updateContentSize();
        update();
    }
}

void JZListWidget::setItemPaint(PaintFn fn)
{
    m_paintFn = std::move(fn);
    update();
}

int JZListWidget::currentIndex() const
{
    return m_current;
}

void JZListWidget::setCurrentIndex(int index)
{
    if (index < 0 || index >= (int)m_items.size())
        return;
    if (index == m_current)
        return;
    m_current = index;
    update();   // 重绘选择态
    currentIndexChanged.emit(index);
}

JZListWidgetItem* JZListWidget::currentItem() const
{
    return item(m_current);   // m_current 为 -1 时自然返回 nullptr
}

void JZListWidget::setCurrentItem(JZListWidgetItem* item, bool emitSignal)
{
    if (!item)
        return;
    int index = row(item);
    if (index < 0 || index == m_current)
        return;
    m_current = index;
    update();
    if (emitSignal)
        currentIndexChanged.emit(index);
}

// ----------------------------------------------------------------------------
// 虚拟列表核心
// ----------------------------------------------------------------------------
int JZListWidget::itemsPerRow() const
{
    if (m_mode == ListMode) return 1;
    int w = viewportWidth();
    if (w <= 0) return 1;
    int n = w / m_cellW;
    return n < 1 ? 1 : n;
}

int JZListWidget::rowHeight() const
{
    return (m_mode == ListMode) ? m_itemHeight : m_cellH;
}

int JZListWidget::cellWidth() const
{
    return (m_mode == ListMode) ? viewportWidth() : m_cellW;
}

void JZListWidget::updateContentSize()
{
    int perRow = itemsPerRow();
    int rows = (perRow == 0) ? 0 : ((int)m_items.size() + perRow - 1) / perRow;
    setContentSize(-1, rows * rowHeight());   // 宽度跟随视口, 仅纵向滚动
}

// 内置默认绘图: 图标 + 文本(列表模式左对齐, 图片模式缩略图 + 居中说明)
void JZListWidget::defaultPaint(JZPainter& p, const JZRect& rect, JZListWidgetItem* item, int index)
{
    bool sel = (index == m_current);
    if (sel)
        p.fillRect(rect.x(), rect.y(), rect.width(), rect.height(), JZColor(0x33, 0x66, 0xcc));

    if (m_mode == ListMode) {
        int iconSize = 20;
        int textX = rect.x() + 6;
        if (!item->icon().isNull()) {
            int ix = rect.x() + 6;
            int iy = rect.y() + (rect.height() - iconSize) / 2;
            p.drawImage(item->icon(), ix, iy, iconSize, iconSize);
            textX = ix + iconSize + 6;
        }
        // drawText 取 pen.color() (非 brush): 选中行白字, 否则黑字
        p.setPen(JZPen(sel ? JZColor::White : JZColor::Black, 1));
        JZFont font(14); p.setFont(font);
        int ty = rect.y() + JZFontMetrics::centerY(font, rect.height());
        p.drawText(textX, ty, item->text().c_str());
    } else {
        int iconSize = rect.height() - 22;
        if (iconSize > 0 && !item->icon().isNull()) {
            int cx = rect.x() + (rect.width() - iconSize) / 2;
            int cy = rect.y() + 2;
            p.drawImage(item->icon(), cx, cy, iconSize, iconSize);
        }
        // drawText 取 pen.color() (非 brush): 选中行白字, 否则黑字
        p.setPen(JZPen(sel ? JZColor::White : JZColor::Black, 1));
        JZFont font(12); p.setFont(font);
        int th = JZFontMetrics::textHeight(font);
        p.drawText(rect.x() + 2, rect.y() + rect.height() - th - 4, item->text().c_str());
    }
}

void JZListWidget::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);

    int viewW = viewportWidth();
    int viewH = viewportHeight();
    int sy = scrollY();

    // 整控件白底 (含滚动条带宽下的底色), 防止项与项之间/滚动条后残留旧帧内容
    p.fillRect(0, 0, width(), height(), JZColor::White);

    int perRow = itemsPerRow();
    int rh = rowHeight();
    int cw = cellWidth();

    int firstRow = (int)sy / rh;
    if (firstRow < 0) firstRow = 0;
    int visRows = (viewH > 0 ? viewH / rh : 0) + 2;

    for (int r = firstRow; r <= firstRow + visRows; r++) {
        for (int c = 0; c < perRow; c++) {
            int index = r * perRow + c;
            if (index >= (int)m_items.size())
                return;
            int cx = (m_mode == ListMode) ? 0 : c * cw;
            int cy = r * rh - sy;
            if (cy + rh < 0 || cy > viewH) continue;
            JZRect rect(cx, cy, (m_mode == ListMode) ? viewW : cw, rh);
            if (m_paintFn)
                m_paintFn(p, rect, m_items[index].get());
            else
                defaultPaint(p, rect, m_items[index].get(), index);
        }
    }
}

// ----------------------------------------------------------------------------
// 事件
// ----------------------------------------------------------------------------

// 点击选择: 视口坐标 + 滚动偏移 -> 内容坐标 -> 行列 -> 项索引
void JZListWidget::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() != JZMouseEvent::LeftButton) {
        JZScrollArea::mousePressEvent(event);
        return;
    }

    const int perRow = itemsPerRow();
    const int rh = rowHeight();
    if (perRow <= 0 || rh <= 0)
        return;

    int row = (event->localPos().y() + scrollY()) / rh;
    int col = (m_mode == ListMode) ? 0 : event->localPos().x() / cellWidth();
    int index = row * perRow + col;
    if (index >= 0 && index < (int)m_items.size()) {
        setCurrentIndex(index);
        sigActivated.emit(index);   // 点击即激活 (下拉弹层收编用)
    }
}

// 键盘: Up/Down 移动选择 (带滚动可见), Enter 激活; Esc ignore 冒泡给弹层关闭
void JZListWidget::keyPressEvent(JZKeyEvent* event)
{
    switch (event->key()) {
    case JZKey::Key_Up:
    case JZKey::Key_Down: {
        if (m_items.empty()) {
            event->accept();
            return;
        }
        int idx = m_current + (event->key() == JZKey::Key_Up ? -1 : 1);
        idx = std::max(0, std::min(idx, (int)m_items.size() - 1));
        if (idx != m_current) {
            setCurrentIndex(idx);
            // 选择行滚入视口
            ensureVisible(idx * rowHeight(), idx * rowHeight() + rowHeight());
        }
        event->accept();
        return;
    }

    case JZKey::Key_Return:
        if (m_current >= 0)
            sigActivated.emit(m_current);
        event->accept();
        return;

    case JZKey::Key_Escape:
        event->ignore();   // 冒泡: Popup 型窗口按 Esc 隐藏
        return;

    default:
        break;
    }
    JZScrollArea::keyPressEvent(event);
}

void JZListWidget::showEvent(JZShowEvent* e)
{
    JZScrollArea::showEvent(e);
    updateContentSize();
    update();
}
