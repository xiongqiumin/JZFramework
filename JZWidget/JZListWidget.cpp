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
// JZListWidgetItem: 默认实现 (图标 + 单行文本) + 改值通知
// ============================================================================
void JZListWidgetItem::setText(const std::string& t)
{
    m_text = t;
    if (m_widget)
    {
        m_widget->itemChanged(this);   // 文本变了高度可能也变了 -> 重测 + 重绘
        m_widget->notifyDataChanged();
    }
}

void JZListWidgetItem::setIcon(const JZImage& i)
{
    m_icon = i;
    if (m_widget)
    {
        m_widget->itemChanged(this);
        m_widget->notifyDataChanged();
    }
}

void JZListWidgetItem::setData(const JZVariant& d)
{
    m_data = d;
    if (m_widget)
    {
        m_widget->itemChanged(this);   // 自定义 itemPaint 可能依据 data 绘制, 故同样重绘
        m_widget->notifyDataChanged();
    }
}

int JZListWidgetItem::heightForWidth(int width)
{
    (void)width;
    return -1;      // 不参与逐项测量: 用列表的 setItemHeight() 定高
}

void JZListWidgetItem::paint(JZPainter& p, const JZRect& rect, const JZListItemPaintInfo& info)
{
    const bool sel = info.selected;

    // 选中底色由项自己画(默认实现画蓝底 + 白字); 自绘项要高亮就同样按 info.selected 自己画
    if (sel)
        p.fillRect(rect.x(), rect.y(), rect.width(), rect.height(), JZColor(0x33, 0x66, 0xcc));

    const int iconSize = 20;
    int textX = rect.x() + 6;
    if (!m_icon.isNull())
    {
        const int ix = rect.x() + 6;
        const int iy = rect.y() + (rect.height() - iconSize) / 2;
        p.drawImage(m_icon, ix, iy, iconSize, iconSize);
        textX = ix + iconSize + 6;
    }
    // drawText 取 pen.color() (非 brush): 选中行白字, 否则黑字
    p.setPen(JZPen(sel ? JZColor::White : JZColor::Black, 1));
    JZFont font(14);
    p.setFont(font);
    const int ty = rect.y() + JZFontMetrics::centerY(font, rect.height());
    p.drawText(textX, ty, m_text.c_str());
}

bool JZListWidgetItem::mousePress(const JZRect& rect, JZMouseEvent* e)
{
    (void)rect; (void)e;
    return false;   // 默认不消费: 列表照旧做"点击选中"
}

bool JZListWidgetItem::mouseMove(const JZRect& rect, JZMouseEvent* e)
{
    (void)rect; (void)e;
    return false;
}

bool JZListWidgetItem::mouseRelease(const JZRect& rect, JZMouseEvent* e)
{
    (void)rect; (void)e;
    return false;
}

bool JZListWidgetItem::keyPress(JZKeyEvent* e)
{
    (void)e;
    return false;
}

void JZListWidgetItem::notifyChanged()
{
    if (m_widget)
        m_widget->itemChanged(this);
}

// ============================================================================
// JZListWidget (owner-draw 虚拟列表, 继承 JZAbstractViewWidget)
// ============================================================================
JZListWidget::JZListWidget(JZWidget* parent)
    : JZAbstractViewWidget(parent)
    , currentIndexChanged(this)
    , sigActivated(this)
    , sigDataChanged(this)
{}

JZListWidget::~JZListWidget()
{
}

// ---- 数据模型 ----

void JZListWidget::invalidateLayout(int fromIndex)
{
    if (!m_layoutValid)
        return;
    const int n = (int)m_items.size();
    const int from = (fromIndex < 0) ? 0 : std::min(fromIndex, n);
    if (from < m_dirtyFrom)
        m_dirtyFrom = from;
}

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

    invalidateLayout(index);                     // 插入点之后的项索引/高度都变了
    updateContentSize();
    update();
    notifyDataChanged();
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
    invalidateLayout(0);
    updateContentSize();
    update();
    notifyDataChanged();
}

JZListWidgetItem* JZListWidget::takeItem(int index)
{
    if (index < 0 || index >= (int)m_items.size())
        return nullptr;

    JZListWidgetItem* raw = m_items[index].release();   // 交出所有权
    raw->m_widget = nullptr;                            // 不再属于本列表
    m_items.erase(m_items.begin() + index);

    const bool hadCurrent = (m_current >= 0);
    if (m_current == index)
        m_current = -1;                                 // 选中项被摘除 -> 清空选择
    else if (m_current > index)
        m_current--;

    invalidateLayout(index);
    updateContentSize();
    update();
    notifyDataChanged();

    // 选中项被摘除 = 选择变成"无": 必须发信号, 否则外部观察者(JS 的 onCurrentIndexChanged)
    // 手里还留着一个已经不存在的旧索引。这是"选择被清空"的唯一出口(没有别的通知途径)。
    // 注意 Qt 语义: 摘除当前项**不会**自动把选择挪到邻项 —— 要保持选中, 由调用方自己再 setCurrentIndex。
    if (hadCurrent && m_current < 0)
        currentIndexChanged.emit(-1);

    return raw;
}

// removeItem 已按 Qt 的做法删除: 要删除某项就 `delete list->takeItem(row)`。

void JZListWidget::clear()
{
    const bool hadCurrent = (m_current >= 0);
    m_items.clear();
    m_current = -1;
    m_layoutValid = false;
    m_dirtyFrom = 0;
    m_heights.clear();
    m_tops.clear();
    updateContentSize();
    update();
    notifyDataChanged();
    if (hadCurrent)
        currentIndexChanged.emit(-1);   // 选择被清空(同上: 摘除选中项后索引变 -1 要通知外部)
}

void JZListWidget::notifyDataChanged()
{
    sigDataChanged.emit();
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

void JZListWidget::setItemHeight(int h)
{
    if (h <= 0) return;
    m_itemHeight = h;
    invalidateLayout(0);
    updateContentSize();
    update();
}

int JZListWidget::itemHeight() const
{
    return m_itemHeight;
}

// ---- 逐项测量 (变高) ----
void JZListWidget::setDynamicItemHeight(bool on)
{
    if (m_dynamic == on)
        return;
    m_dynamic = on;
    invalidateLayout(0);
    updateContentSize();
    update();
}

bool JZListWidget::dynamicItemHeight() const
{
    return m_dynamic;
}

void JZListWidget::setSpacing(int px)
{
    const int v = std::max(0, px);
    if (v == m_spacing)
        return;
    m_spacing = v;
    invalidateLayout(0);
    if (m_dynamic) {
        updateContentSize();
        update();
    }
}

int JZListWidget::spacing() const
{
    return m_spacing;
}

void JZListWidget::setContentPadding(int left, int top, int right, int bottom)
{
    m_padL = std::max(0, left);
    m_padT = std::max(0, top);
    m_padR = std::max(0, right);
    m_padB = std::max(0, bottom);
    invalidateLayout(0);
    if (m_dynamic) {
        updateContentSize();
        update();
    }
}

int JZListWidget::contentPaddingLeft() const { return m_padL; }
int JZListWidget::contentPaddingTop() const { return m_padT; }
int JZListWidget::contentPaddingRight() const { return m_padR; }
int JZListWidget::contentPaddingBottom() const { return m_padB; }

void JZListWidget::setStickToBottom(bool on)
{
    m_stickBottom = on;
}

bool JZListWidget::stickToBottom() const
{
    return m_stickBottom;
}

bool JZListWidget::atBottom() const
{
    return scrollY() >= maxScrollY() - 1;
}

void JZListWidget::setBackgroundColor(const JZColor& c)
{
    m_bgColor = c;
    update();
}

void JZListWidget::setEmptyText(const std::string& text)
{
    m_emptyText = text;
    update();
}

// ---- 内部布局: 前缀和 ----
int JZListWidget::contentWidthForItems() const
{
    const int w = viewportWidth() - m_padL - m_padR;
    return std::max(1, w);
}

// 惰性重算: 宽度变了/项数变了 -> 全量; 只有某项内容变了 -> 从该项起重算。
// 逐项测量只在"变高 + 列表模式"下生效; 图片模式仍是网格定高。
void JZListWidget::ensureLayout() const
{
    const int n = (int)m_items.size();
    const int w = contentWidthForItems();
    const bool dynamic = m_dynamic;
    const bool needAll = !m_layoutValid || (w != m_layoutWidth) || ((int)m_heights.size() != n);

    if (!needAll && m_dirtyFrom >= n)
        return;                                  // 全部有效

    if (needAll)
    {
        m_heights.assign((size_t)n, m_itemHeight);
        m_tops.assign((size_t)n + 1, 0);
        m_layoutWidth = w;
        m_layoutValid = true;
        m_dirtyFrom = 0;
    }

    const int from = std::max(0, std::min(m_dirtyFrom, n));
    if (from == 0)
        m_tops[0] = 0;

    for (int i = from; i < n; i++)
    {
        int h = -1;
        if (dynamic)
            h = m_items[i]->heightForWidth(w);
        if (h < 0)
            h = m_itemHeight;                    // 未声明自定高 -> 用列表定高
        m_heights[i] = h;
    }
    for (int i = from; i < n; i++)
        m_tops[i + 1] = m_tops[i] + m_heights[i] + ((i + 1 < n) ? m_spacing : 0);

    m_dirtyFrom = n;
}

int JZListWidget::itemTop(int index) const
{
    if (index < 0 || index >= (int)m_items.size())
        return -1;
    ensureLayout();
    return m_padT + m_tops[index];
}

int JZListWidget::itemHeightAt(int index) const
{
    if (index < 0 || index >= (int)m_items.size())
        return -1;
    ensureLayout();
    return m_heights[index];
}

int JZListWidget::indexAt(const JZPoint& contentPos) const
{
    const int n = (int)m_items.size();
    if (n == 0)
        return -1;

    if (m_dynamic)
    {
        ensureLayout();
        const int y = contentPos.y() - m_padT;
        if (y < 0 || contentPos.x() < m_padL || contentPos.x() > m_padL + contentWidthForItems())
            return -1;
        // 二分: 最后一个 tops[i] <= y 的项
        int lo = 0, hi = n - 1, ans = -1;
        while (lo <= hi)
        {
            const int mid = (lo + hi) / 2;
            if (m_tops[mid] <= y) { ans = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        if (ans < 0)
            return -1;
        if (y >= m_tops[ans] + m_heights[ans])
            return -1;                            // 落在项间距里
        return ans;
    }

    if (m_itemHeight <= 0)
        return -1;
    const int idx = contentPos.y() / m_itemHeight;
    return (idx >= 0 && idx < n) ? idx : -1;
}

bool JZListWidget::itemRect(int index, JZRect& out) const
{
    if (index < 0 || index >= (int)m_items.size())
        return false;

    const int sy = scrollY();
    if (m_dynamic)
    {
        ensureLayout();
        out = JZRect(m_padL, m_padT + m_tops[index] - sy,
                     contentWidthForItems(), m_heights[index]);
        return true;
    }

    out = JZRect(0, index * m_itemHeight - sy, viewportWidth(), m_itemHeight);
    return true;
}

void JZListWidget::itemChanged(JZListWidgetItem* item)
{
    const int i = row(item);
    if (i < 0)
        return;
    invalidateLayout(i);       // 只从这一项起重算(高度前缀和是整数加法, 很便宜)
    updateContentSize();
    update();
}

JZSize JZListWidget::sizeHint() const
{
    // 虚拟列表自己知道该多大:
    //   宽度 = 最宽项 (文本 + 图标) + 左右留白
    //   高度 = 行数 * 行高, 至多 8 行 (再多交给滚动 —— 与下拉弹层的可见行数一致)
    const int maxVisible = 8;

    JZFont font(14);
    int w = 0;
    for (size_t i = 0; i < m_items.size(); i++)
    {
        const JZListWidgetItem& it = *m_items[i];
        int iw = JZFontMetrics::textWidth(it.text(), font);
        if (!it.icon().isNull())
            iw += 20 + 6;              // 图标 + 间距 (与默认绘制一致)
        if (iw > w)
            w = iw;
    }
    w += 6 + 6;                        // 默认绘制左右各留 6

    const int rows = std::min(std::max(1, (int)m_items.size()), maxVisible);
    const int h = rows * m_itemHeight + 4;

    const JZSize mn = minimumSizeHint();
    return JZSize(std::max(w, mn.width()), std::max(h, mn.height()));
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
    const int index = row(item);
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
void JZListWidget::updateContentSize()
{
    // "变化前是否贴底"要在 setContentSize 之前取: 内容长高后 maxScrollY 会变
    const bool wasBottom = atBottom();

    ensureLayout();

    if (m_dynamic)
    {
        const int n = (int)m_items.size();
        setContentSize(-1, m_padT + (n ? m_tops[n] : 0) + m_padB);
    }
    else
    {
        setContentSize(-1, (int)m_items.size() * m_itemHeight);   // 宽度跟随视口, 仅纵向滚动
    }

    if (m_stickBottom && wasBottom)
        scrollToBottom();                        // 保持贴底(用户上滚看历史时不会被拽走)
}

void JZListWidget::paintItemAt(JZPainter& p, int index, const JZRect& rect)
{
    JZListWidgetItem* it = m_items[index].get();
    if (m_paintFn) {
        m_paintFn(p, rect, it);                  // 整体换画法(旧接口)
        return;
    }
    JZListItemPaintInfo info;
    info.index = index;
    info.selected = (index == m_current);
    it->paint(p, rect, info);
}

void JZListWidget::paintEvent(JZPainterEvent* event)
{
    (void)event;
    JZPainter p(this);

    const int viewW = viewportWidth();
    const int viewH = viewportHeight();
    const int sy = scrollY();

    p.fillRect(0, 0, width(), height(), m_bgColor);

    if (m_items.empty())
    {
        if (!m_emptyText.empty())
        {
            JZFont font(14);
            p.setPen(JZPen(JZColor(0x99, 0x99, 0x99), 1));
            p.setFont(font);
            const int tx = JZFontMetrics::centerX(m_emptyText, font, width());
            p.drawText(tx, JZFontMetrics::centerY(font, height()), m_emptyText.c_str());
        }
        return;
    }

    // ---- 逐项测量模式: 二分找首个可见项, 再顺序画到视口底 ----
    if (m_dynamic)
    {
        ensureLayout();
        const int n = (int)m_items.size();
        int first = n;
        {
            int lo = 0, hi = n - 1;
            while (lo <= hi)
            {
                const int mid = (lo + hi) / 2;
                if (m_padT + m_tops[mid] + m_heights[mid] > sy) { first = mid; hi = mid - 1; }
                else lo = mid + 1;
            }
        }
        const int w = contentWidthForItems();
        for (int i = first; i < n; i++)
        {
            const int y = m_padT + m_tops[i] - sy;
            const int h = m_heights[i];
            if (y > viewH)
                break;
            if (y + h <= 0)
                continue;
            paintItemAt(p, i, JZRect(m_padL, y, w, h));
        }
        return;
    }

    // ---- 定高老路径 ----
    const int rh = m_itemHeight;
    if (rh <= 0)
        return;

    const int firstRow = std::max(0, sy / rh);
    const int visRows = (viewH > 0 ? viewH / rh : 0) + 2;

    for (int r = firstRow; r <= firstRow + visRows; r++) {
        const int index = r;
        if (index >= (int)m_items.size())
            return;
        const int cy = r * rh - sy;
        if (cy + rh < 0 || cy > viewH) continue;
        paintItemAt(p, index, JZRect(0, cy, viewW, rh));
    }
}

void JZListWidget::resizeEvent(JZResizeEvent* event)
{
    invalidateLayout(0);        // 宽度变化会让换行结果变, 必须全量重测
    JZAbstractViewWidget::resizeEvent(event);   // 基类会回调 updateContentSize()
    update();
}

// ----------------------------------------------------------------------------
// 事件
// ----------------------------------------------------------------------------

// 点击: 先给项自己判(折叠/拖选/链接), 没消费才走"选中 + 激活"
void JZListWidget::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() != JZMouseEvent::LeftButton) {
        JZScrollArea::mousePressEvent(event);
        return;
    }

    const JZPoint contentPos(event->localPos().x(), event->localPos().y() + scrollY());
    const int index = indexAt(contentPos);
    if (index < 0) {
        JZScrollArea::mousePressEvent(event);
        return;
    }

    JZRect rect;
    if (itemRect(index, rect) && m_items[index]->mousePress(rect, event)) {
        m_mouseItem = index;          // 后续 move/release 继续发给这一项
        event->accept();
        return;
    }

    setCurrentIndex(index);
    sigActivated.emit(index);   // 点击即激活 (下拉弹层收编用)
}

void JZListWidget::mouseMoveEvent(JZMouseEvent* event)
{
    if (m_mouseItem >= 0 && m_mouseItem < (int)m_items.size())
    {
        JZRect rect;
        if (itemRect(m_mouseItem, rect) && m_items[m_mouseItem]->mouseMove(rect, event)) {
            event->accept();
            return;
        }
    }
    JZScrollArea::mouseMoveEvent(event);
}

void JZListWidget::mouseReleaseEvent(JZMouseEvent* event)
{
    if (m_mouseItem >= 0)
    {
        const int i = m_mouseItem;
        m_mouseItem = -1;
        if (i < (int)m_items.size())
        {
            JZRect rect;
            if (itemRect(i, rect))
                m_items[i]->mouseRelease(rect, event);
        }
    }
    JZScrollArea::mouseReleaseEvent(event);
}

// 键盘: 先给当前项(如 Ctrl+C 复制选区), 再走 Up/Down 移动选择、Enter 激活
void JZListWidget::keyPressEvent(JZKeyEvent* event)
{
    if (m_current >= 0 && m_current < (int)m_items.size()
        && m_items[m_current]->keyPress(event))
    {
        event->accept();
        return;
    }

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
            // 选择行滚入视口 (变高模式下用真实顶部/高度)
            const int top = itemTop(idx);
            ensureVisible(top, top + itemHeightAt(idx));
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
