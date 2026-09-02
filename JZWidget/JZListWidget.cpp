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

JZ_OBJECT_IMPL(JZListWidget, JZScrollArea)

// ============================================================================
// JZListWidget (owner-draw 虚拟列表, 继承 JZScrollArea)
// ============================================================================
JZListWidget::JZListWidget(JZWidget* parent)
    : JZScrollArea(parent)
    , currentIndexChanged(this)
    , sigActivated(this)
{}

JZListWidget::~JZListWidget()
{
}

void JZListWidget::addItem(const std::string& text)
{
    JZListWidgetItem it;
    it.widget = this;
    it.text = text;
    m_items.push_back(std::move(it));
    updateContentSize();
    update();
}

void JZListWidget::addItem(const JZListWidgetItem& item)
{
    JZListWidgetItem it = item;
    it.widget = this;
    m_items.push_back(std::move(it));
    updateContentSize();
    update();
}

void JZListWidget::addItems(const std::vector<std::string>& texts)
{
    for (const auto& t : texts) {
        JZListWidgetItem it;
        it.widget = this;
        it.text = t;
        m_items.push_back(std::move(it));
    }
    updateContentSize();
    update();
}

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

const JZListWidgetItem& JZListWidget::item(int index) const
{
    static JZListWidgetItem s_empty;
    if (index < 0 || index >= (int)m_items.size())
        return s_empty;
    return m_items[index];
}

std::string JZListWidget::itemText(int index) const
{
    if (index < 0 || index >= (int)m_items.size())
        return {};
    return m_items[index].text;
}

JZVariant JZListWidget::itemData(int index) const
{
    if (index < 0 || index >= (int)m_items.size())
        return JZVariant();
    return m_items[index].data;
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
        if (!item->icon.isNull()) {
            int ix = rect.x() + 6;
            int iy = rect.y() + (rect.height() - iconSize) / 2;
            p.drawImage(item->icon, ix, iy, iconSize, iconSize);
            textX = ix + iconSize + 6;
        }
        JZBrush brush; brush.setColor(sel ? JZColor::White: JZColor::Black); p.setBrush(brush);
        JZFont font(14); p.setFont(font);
        int ty = rect.y() + JZFontMetrics::centerY(font, rect.height());
        p.drawText(textX, ty, item->text.c_str());
    } else {
        int iconSize = rect.height() - 22;
        if (iconSize > 0 && !item->icon.isNull()) {
            int cx = rect.x() + (rect.width() - iconSize) / 2;
            int cy = rect.y() + 2;
            p.drawImage(item->icon, cx, cy, iconSize, iconSize);
        }
        JZBrush brush; brush.setColor(sel ? JZColor::White : JZColor::Black); p.setBrush(brush);
        JZFont font(12); p.setFont(font);
        int th = JZFontMetrics::textHeight(font);
        p.drawText(rect.x() + 2, rect.y() + rect.height() - th - 4, item->text.c_str());
    }
}

void JZListWidget::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);

    int viewW = viewportWidth();
    int viewH = viewportHeight();
    int sy = scrollY();

    // 视口白底, 防止项与项之间残留旧帧内容
    p.fillRect(0, 0, viewW, viewH, JZColor::White);

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
                m_paintFn(p, rect, &m_items[index]);
            else
                defaultPaint(p, rect, &m_items[index], index);
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
    case VK_UP:
    case VK_DOWN: {
        if (m_items.empty()) {
            event->accept();
            return;
        }
        int idx = m_current + (event->key() == VK_UP ? -1 : 1);
        idx = std::max(0, std::min(idx, (int)m_items.size() - 1));
        if (idx != m_current) {
            setCurrentIndex(idx);

            // 选择行滚入视口
            int top = idx * rowHeight();
            int bottom = top + rowHeight();
            if (top < scrollY())
                scrollToY(top);
            else if (bottom > scrollY() + viewportHeight())
                scrollToY(bottom - viewportHeight());
        }
        event->accept();
        return;
    }

    case VK_RETURN:
        if (m_current >= 0)
            sigActivated.emit(m_current);
        event->accept();
        return;

    case VK_ESCAPE:
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
    update();
}

void JZListWidget::resizeEvent(JZResizeEvent* e)
{
    JZScrollArea::resizeEvent(e);
    updateContentSize();
    update();
}
