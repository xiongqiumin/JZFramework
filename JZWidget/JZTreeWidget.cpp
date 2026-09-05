#include "JZTreeWidget.h"

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

JZ_OBJECT_IMPL(JZTreeWidget, JZAbstractViewWidget)

// ============================================================================
// JZTreeWidgetItem
// ============================================================================
JZTreeWidgetItem::JZTreeWidgetItem()
{
}

JZTreeWidgetItem::JZTreeWidgetItem(const std::string& text)
    : m_text(text)
{
}

JZTreeWidgetItem::~JZTreeWidgetItem()
{
    for (auto* c : m_children)
        delete c;
    m_children.clear();
}

void JZTreeWidgetItem::setText(const std::string& text)
{
    m_text = text;
    if (m_widget)
        m_widget->itemChanged(this);
}

void JZTreeWidgetItem::setIcon(const JZImage& icon)
{
    m_icon = icon;
    if (m_widget)
        m_widget->itemChanged(this);
}

void JZTreeWidgetItem::setWidget(JZTreeWidget* w)
{
    m_widget = w;
    for (auto* c : m_children)
        c->setWidget(w);
}

void JZTreeWidgetItem::addChild(JZTreeWidgetItem* child)
{
    if (!child || child->m_parent || child->m_widget)
        return;
    child->m_parent = this;
    child->setWidget(m_widget);
    m_children.push_back(child);
    if (m_widget) m_widget->rebuildVisible();
}

void JZTreeWidgetItem::insertChild(int index, JZTreeWidgetItem* child)
{
    if (!child || child->m_parent || child->m_widget)
        return;
    child->m_parent = this;
    child->setWidget(m_widget);
    if (index < 0 || index >= (int)m_children.size())
        m_children.push_back(child);
    else
        m_children.insert(m_children.begin() + index, child);
    if (m_widget) m_widget->rebuildVisible();
}

JZTreeWidgetItem* JZTreeWidgetItem::takeChild(int index)
{
    if (index < 0 || index >= (int)m_children.size())
        return nullptr;
    JZTreeWidgetItem* c = m_children[index];
    m_children.erase(m_children.begin() + index);
    c->m_parent = nullptr;
    c->setWidget(nullptr);
    if (m_widget) m_widget->rebuildVisible();
    return c;
}

void JZTreeWidgetItem::removeChild(JZTreeWidgetItem* child)
{
    auto it = std::find(m_children.begin(), m_children.end(), child);
    if (it == m_children.end())
        return;
    m_children.erase(it);
    child->m_parent = nullptr;
    child->setWidget(nullptr);
    if (m_widget) m_widget->rebuildVisible();
}

JZTreeWidgetItem* JZTreeWidgetItem::child(int index) const
{
    if (index < 0 || index >= (int)m_children.size())
        return nullptr;
    return m_children[index];
}

int JZTreeWidgetItem::indexOfChild(const JZTreeWidgetItem* child) const
{
    auto it = std::find(m_children.begin(), m_children.end(), child);
    return (it == m_children.end()) ? -1 : (int)(it - m_children.begin());
}

int JZTreeWidgetItem::childIndex() const
{
    if (!m_parent)
        return -1;
    return m_parent->indexOfChild(this);
}

void JZTreeWidgetItem::setExpanded(bool exp)
{
    if (exp == m_expanded)
        return;
    m_expanded = exp;
    if (m_widget) {
        m_widget->rebuildVisible();
        if (m_expanded)
            m_widget->itemExpanded.emit(this);
        else
            m_widget->itemCollapsed.emit(this);
    }
}

// ============================================================================
// JZTreeWidget (owner-draw 虚拟树, 继承 JZAbstractViewWidget)
// ============================================================================
JZTreeWidget::JZTreeWidget(JZWidget* parent)
    : JZAbstractViewWidget(parent)
    , currentItemChanged(this)
    , itemActivated(this)
    , itemExpanded(this)
    , itemCollapsed(this)
{
    setScrollDirection(JZScrollArea::ScrollBarAsNeeded);
}

JZTreeWidget::~JZTreeWidget()
{
    clear();
}

// ----------------------------------------------------------------------------
// 数据模型
// ----------------------------------------------------------------------------
void JZTreeWidget::addTopLevelItem(JZTreeWidgetItem* item)
{
    if (!item || item->m_parent || item->m_widget)
        return;
    item->m_parent = nullptr;
    item->setWidget(this);
    m_topItems.push_back(item);
    rebuildVisible();
}

void JZTreeWidget::addTopLevelItems(const std::vector<JZTreeWidgetItem*>& items)
{
    for (auto* it : items)
        addTopLevelItem(it);
}

void JZTreeWidget::insertTopLevelItem(int index, JZTreeWidgetItem* item)
{
    if (!item || item->m_parent || item->m_widget)
        return;
    item->m_parent = nullptr;
    item->setWidget(this);
    if (index < 0 || index >= (int)m_topItems.size())
        m_topItems.push_back(item);
    else
        m_topItems.insert(m_topItems.begin() + index, item);
    rebuildVisible();
}

JZTreeWidgetItem* JZTreeWidget::takeTopLevelItem(int index)
{
    if (index < 0 || index >= (int)m_topItems.size())
        return nullptr;
    JZTreeWidgetItem* it = m_topItems[index];
    m_topItems.erase(m_topItems.begin() + index);
    it->m_parent = nullptr;
    it->setWidget(nullptr);
    if (m_current == it)
        m_current = nullptr;
    rebuildVisible();
    return it;
}

int JZTreeWidget::indexOfTopLevelItem(JZTreeWidgetItem* item) const
{
    auto it = std::find(m_topItems.begin(), m_topItems.end(), item);
    return (it == m_topItems.end()) ? -1 : (int)(it - m_topItems.begin());
}

void JZTreeWidget::clear()
{
    for (auto* it : m_topItems)
        delete it;
    m_topItems.clear();
    m_visible.clear();
    m_current = nullptr;
    updateContentSize();
    update();
}

JZTreeWidgetItem* JZTreeWidget::topLevelItem(int index) const
{
    if (index < 0 || index >= (int)m_topItems.size())
        return nullptr;
    return m_topItems[index];
}

// 在可见行序列中定位 item 的上一/下一行
JZTreeWidgetItem* JZTreeWidget::itemAbove(JZTreeWidgetItem* item) const
{
    int r = rowOfItem(item);
    if (r < 0 || r - 1 < 0)
        return nullptr;
    return m_visible[r - 1].item;
}

JZTreeWidgetItem* JZTreeWidget::itemBelow(JZTreeWidgetItem* item) const
{
    int r = rowOfItem(item);
    if (r < 0 || r + 1 >= (int)m_visible.size())
        return nullptr;
    return m_visible[r + 1].item;
}

// ----------------------------------------------------------------------------
// 视图
// ----------------------------------------------------------------------------
void JZTreeWidget::setHeaderLabel(const std::string& label)
{
    m_header = label;
    updateContentSize();
    update();
}

void JZTreeWidget::setRowHeight(int h)
{
    if (h <= 0) return;
    m_rowHeight = h;
    updateContentSize();
    update();
}

void JZTreeWidget::setIndentation(int px)
{
    if (px < 0) return;
    m_indent = px;
    update();
}

void JZTreeWidget::setItemPaint(PaintFn fn)
{
    m_paintFn = std::move(fn);
    update();
}

void JZTreeWidget::itemChanged(JZTreeWidgetItem* item)
{
    (void)item;
    update();
}

// ----------------------------------------------------------------------------
// 选择
// ----------------------------------------------------------------------------
int JZTreeWidget::currentIndex() const
{
    return rowOfItem(m_current);
}

void JZTreeWidget::setCurrentItem(JZTreeWidgetItem* item, bool emitSignal)
{
    if (item == m_current)
        return;
    // 仅允许选中当前可见的项
    if (item && rowOfItem(item) < 0)
        return;
    JZTreeWidgetItem* prev = m_current;
    m_current = item;
    update();
    if (emitSignal)
        currentItemChanged.emit(m_current);
    (void)prev;
}

// ----------------------------------------------------------------------------
// 展开/折叠
// ----------------------------------------------------------------------------
void JZTreeWidget::expandAll()
{
    for (auto* it : m_topItems)
        expandItem(it);
}

void JZTreeWidget::collapseAll()
{
    for (auto* it : m_topItems)
        collapseItem(it);
}

void JZTreeWidget::expandItem(JZTreeWidgetItem* item)
{
    if (!item || item->m_expanded || item->m_children.empty())
        return;
    item->m_expanded = true;
    rebuildVisible();
    itemExpanded.emit(item);
}

void JZTreeWidget::collapseItem(JZTreeWidgetItem* item)
{
    if (!item || !item->m_expanded || item->m_children.empty())
        return;
    item->m_expanded = false;
    // 折叠后当前项若落入隐藏子树, 则将当前项上移到可见的折叠节点
    if (m_current && m_current != item) {
        JZTreeWidgetItem* p = m_current->m_parent;
        bool hidden = false;
        while (p) {
            if (p == item) { hidden = true; break; }
            p = p->m_parent;
        }
        if (hidden) {
            JZTreeWidgetItem* prev = m_current;
            m_current = item;
            currentItemChanged.emit(item);
            (void)prev;
        }
    }
    rebuildVisible();
    itemCollapsed.emit(item);
}

// ----------------------------------------------------------------------------
// 可见行维护
// ----------------------------------------------------------------------------
void JZTreeWidget::rebuildVisible()
{
    m_visible.clear();
    for (auto* it : m_topItems)
        it->setDepth(0);

    std::function<void(JZTreeWidgetItem*, int)> walk = [&](JZTreeWidgetItem* node, int depth) {
        node->setDepth(depth);
        m_visible.push_back({node, depth});
        if (node->m_expanded) {
            for (auto* c : node->m_children)
                walk(c, depth + 1);
        }
    };
    for (auto* it : m_topItems)
        walk(it, 0);

    updateContentSize();
    update();
}

void JZTreeWidget::updateContentSize()
{
    int hh = m_header.empty() ? 0 : m_rowHeight;
    setContentSize(-1, hh + m_rowHeight * (int)m_visible.size());   // 宽度跟随视口, 仅纵向滚动
}

JZTreeWidgetItem* JZTreeWidget::itemAtRow(int row) const
{
    if (row < 0 || row >= (int)m_visible.size())
        return nullptr;
    return m_visible[row].item;
}

int JZTreeWidget::rowOfItem(JZTreeWidgetItem* item) const
{
    if (!item) return -1;
    for (size_t i = 0; i < m_visible.size(); i++) {
        if (m_visible[i].item == item)
            return (int)i;
    }
    return -1;
}

void JZTreeWidget::ensureRowVisible(int row)
{
    if (row < 0 || row >= (int)m_visible.size())
        return;
    int hh = m_header.empty() ? 0 : m_rowHeight;
    ensureVisible(hh + row * m_rowHeight, hh + row * m_rowHeight + m_rowHeight);
}

// ----------------------------------------------------------------------------
// 绘制
// ----------------------------------------------------------------------------
void JZTreeWidget::defaultPaint(JZPainter& p, const JZRect& rect,
                                JZTreeWidgetItem* item, int depth, bool expanded, bool selected)
{
    if (selected)
        p.fillRect(rect.x(), rect.y(), rect.width(), rect.height(), JZColor(0x33, 0x66, 0xcc));

    int x = rect.x() + depth * m_indent;
    int cy = rect.y() + rect.height() / 2;

    // 折叠三角(有子节点才画)
    if (!item->m_children.empty()) {
        int ts = 8;   // 三角边长
        int tx = x + 4;
        int ty = cy;
        JZBrush brush; brush.setColor(selected ? JZColor::White : JZColor::Black); p.setBrush(brush);
        if (expanded) {
            // 朝下三角
            p.drawLine(tx, ty - ts / 2, tx + ts, ty - ts / 2);
            p.drawLine(tx + ts, ty - ts / 2, tx + ts / 2, ty + ts / 2);
            p.drawLine(tx + ts / 2, ty + ts / 2, tx, ty - ts / 2);
        } else {
            // 朝右三角
            p.drawLine(tx, ty - ts / 2, tx, ty + ts / 2);
            p.drawLine(tx, ty + ts / 2, tx + ts, ty);
            p.drawLine(tx + ts, ty, tx, ty - ts / 2);
        }
        x += m_toggleZone;
    }

    int textX = x + 2;
    if (!item->m_icon.isNull()) {
        int iconSize = rect.height() - 6;
        p.drawImage(item->m_icon, x + 2, rect.y() + (rect.height() - iconSize) / 2, iconSize, iconSize);
        textX = x + 2 + iconSize + 4;
    }

    JZBrush brush; brush.setColor(selected ? JZColor::White : JZColor::Black); p.setBrush(brush);
    JZFont font(14); p.setFont(font);
    int ty = rect.y() + JZFontMetrics::centerY(font, rect.height());
    p.drawText(textX, ty, item->text().c_str());
}

void JZTreeWidget::paintHeader(JZPainter& p)
{
    int viewW = viewportWidth();
    int hh = m_rowHeight;
    p.fillRect(0, 0, width(), hh, JZColor(0xe8, 0xe8, 0xe8));
    JZBrush brush; brush.setColor(JZColor::Black); p.setBrush(brush);
    JZFont font(13); p.setFont(font);
    p.drawText(6, JZFontMetrics::centerY(font, hh), m_header.c_str());
    JZPen hpen(JZColor(0x99, 0x99, 0x99), 1); p.setPen(hpen);
    p.drawLine(0, hh - 1, viewW, hh - 1);
}

void JZTreeWidget::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);

    int sx = scrollX();
    int sy = scrollY();
    int viewW = viewportWidth();
    int viewH = viewportHeight();
    int hh = m_header.empty() ? 0 : m_rowHeight;

    // 白底铺满整控件(含滚动条带宽), 防止项与项之间/滚动条后残留旧帧内容
    p.fillRect(0, hh, width(), std::max(0, height() - hh), JZColor::White);

    int firstRow = (int)(sy - hh) / m_rowHeight;
    if (firstRow < 0) firstRow = 0;
    int visRows = (viewH > 0 ? viewH / m_rowHeight : 0) + 2;

    for (int r = firstRow; r <= firstRow + visRows; r++) {
        if (r < 0 || r >= (int)m_visible.size())
            break;
        int cy = hh + r * m_rowHeight - sy;
        if (cy + m_rowHeight < hh || cy > viewH) continue;
        JZRect rect(0, cy, viewW, m_rowHeight);
        auto& row = m_visible[r];
        if (m_paintFn)
            m_paintFn(p, rect, row.item, row.depth, row.item->m_expanded);
        else
            defaultPaint(p, rect, row.item, row.depth, row.item->m_expanded,
                         row.item == m_current);
    }

    if (!m_header.empty())
        paintHeader(p);
}

// ----------------------------------------------------------------------------
// 事件
// ----------------------------------------------------------------------------
void JZTreeWidget::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() != JZMouseEvent::LeftButton) {
        JZScrollArea::mousePressEvent(event);
        return;
    }

    int hh = m_header.empty() ? 0 : m_rowHeight;
    int y = event->localPos().y();
    int x = event->localPos().x();
    int row = (y - hh + scrollY()) / m_rowHeight;
    if (row < 0 || row >= (int)m_visible.size()) {
        JZScrollArea::mousePressEvent(event);
        return;
    }

    auto& vis = m_visible[row];
    JZTreeWidgetItem* item = vis.item;

    // 点击折叠三角区域 -> 切换展开/折叠
    int toggleX0 = vis.depth * m_indent;
    int toggleX1 = toggleX0 + m_toggleZone;
    if (!item->m_children.empty() && x >= toggleX0 && x <= toggleX1) {
        if (item->m_expanded)
            collapseItem(item);
        else
            expandItem(item);
        event->accept();
        return;
    }

    // 否则选中
    setCurrentItem(item);
    event->accept();
}

void JZTreeWidget::keyPressEvent(JZKeyEvent* event)
{
    switch (event->key()) {
    case VK_UP:
    case VK_DOWN: {
        if (m_visible.empty()) { event->accept(); return; }
        int r = rowOfItem(m_current);
        int idx = (r < 0) ? 0 : r + (event->key() == VK_UP ? -1 : 1);
        idx = std::max(0, std::min(idx, (int)m_visible.size() - 1));
        JZTreeWidgetItem* it = m_visible[idx].item;
        setCurrentItem(it);
        ensureRowVisible(idx);
        event->accept();
        return;
    }

    case VK_LEFT: {
        if (!m_current) { event->accept(); return; }
        if (!m_current->m_children.empty() && m_current->m_expanded)
            collapseItem(m_current);
        else if (m_current->m_parent)
            setCurrentItem(m_current->m_parent);
        event->accept();
        return;
    }

    case VK_RIGHT: {
        if (!m_current) { event->accept(); return; }
        if (!m_current->m_children.empty() && !m_current->m_expanded)
            expandItem(m_current);
        else if (!m_current->m_children.empty()) {
            int idx = rowOfItem(m_current) + 1;
            if (idx < (int)m_visible.size()) {
                setCurrentItem(m_visible[idx].item);
                ensureRowVisible(idx);
            }
        }
        event->accept();
        return;
    }

    case VK_RETURN:
        if (m_current)
            itemActivated.emit(m_current);
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

void JZTreeWidget::showEvent(JZShowEvent* e)
{
    JZScrollArea::showEvent(e);
    updateContentSize();
    update();
}
