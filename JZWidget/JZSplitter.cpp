#include "JZSplitter.h"
#include "JZPainter.h"
#include <algorithm>

JZ_OBJECT_IMPL(JZSplitter, JZWidget)

JZSplitter::JZSplitter(Orientation o, JZWidget* parent)
    : JZWidget(parent)
    , sigSplitterMoved(this)
{
    m_orientation = o;
    // 不使用 m_layout: 本控件自行管理子控件几何
}

// ==================== 方向与基本属性 ====================
void JZSplitter::setOrientation(Orientation o)
{
    if (m_orientation == o)
        return;
    m_orientation = o;
    relayout();
    update();
}

JZSplitter::Orientation JZSplitter::orientation() const
{
    return m_orientation;
}

void JZSplitter::setHandleWidth(int w)
{
    m_handleWidth = std::max(1, w);
    relayout();
    update();
}

int JZSplitter::handleWidth() const
{
    return m_handleWidth;
}

// ==================== 子控件管理 ====================
void JZSplitter::addWidget(JZWidget* widget)
{
    insertWidget((int)m_items.size(), widget);
}

void JZSplitter::insertWidget(int index, JZWidget* widget)
{
    if (!widget)
        return;
    index = std::max(0, std::min(index, (int)m_items.size()));
    widget->setParentWidget(this);   // 挂为渲染子节点

    Item it;
    it.widget = widget;
    it.stretch = 1;
    it.collapsible = true;
    m_items.insert(m_items.begin() + index, it);
    m_sizes.insert(m_sizes.begin() + index, 0);   // 由 relayout 计算实际尺寸

    relayout();
    update();
}

void JZSplitter::removeWidget(JZWidget* widget)
{
    int i = indexOf(widget);
    if (i < 0)
        return;
    widget->setParentWidget(nullptr);   // 脱离渲染树 (调用方仍持有所有权)
    m_items.erase(m_items.begin() + i);
    m_sizes.erase(m_sizes.begin() + i);
    relayout();
    update();
}

int JZSplitter::count() const
{
    return (int)m_items.size();
}

JZWidget* JZSplitter::widget(int index) const
{
    if (index < 0 || index >= (int)m_items.size())
        return nullptr;
    return m_items[index].widget;
}

int JZSplitter::indexOf(JZWidget* widget) const
{
    for (int i = 0; i < (int)m_items.size(); ++i)
        if (m_items[i].widget == widget)
            return i;
    return -1;
}

void JZSplitter::setStretchFactor(int index, int stretch)
{
    if (index < 0 || index >= (int)m_items.size())
        return;
    m_items[index].stretch = std::max(0, stretch);
    relayout();
    update();
}

int JZSplitter::stretchFactor(int index) const
{
    if (index < 0 || index >= (int)m_items.size())
        return 0;
    return m_items[index].stretch;
}

void JZSplitter::setCollapsible(int index, bool collapsible)
{
    if (index < 0 || index >= (int)m_items.size())
        return;
    m_items[index].collapsible = collapsible;
}

bool JZSplitter::isCollapsible(int index) const
{
    if (index < 0 || index >= (int)m_items.size())
        return true;
    return m_items[index].collapsible;
}

void JZSplitter::setSizes(const std::vector<int>& sizes)
{
    if ((int)sizes.size() != (int)m_items.size())
        return;
    m_sizes = sizes;
    relayout();
    update();
}

std::vector<int> JZSplitter::sizes() const
{
    return m_sizes;
}

// ==================== 期望尺寸 ====================
JZSize JZSplitter::sizeHint() const
{
    int n = (int)m_items.size();
    int handles = m_handleWidth * (n > 0 ? n - 1 : 0);
    int w = 0, h = 0;
    for (auto& it : m_items) {
        JZSize hs = it.widget->sizeHint();
        if (m_orientation == Horizontal) {
            w += hs.width();
            h = std::max(h, hs.height());
        } else {
            h += hs.height();
            w = std::max(w, hs.width());
        }
    }
    if (m_orientation == Horizontal)
        w += handles;
    else
        h += handles;
    return JZSize(w, h);
}

// ==================== 布局 ====================
void JZSplitter::resize(int w, int h)
{
    JZWidget::resize(w, h);   // 设定自身尺寸并并入脏区
    relayout();               // 按新尺寸重排子控件
}

int JZSplitter::totalStretch() const
{
    int ts = 0;
    for (auto& it : m_items)
        ts += std::max(0, it.stretch);
    return ts;
}

int JZSplitter::minOf(int index) const
{
    if (index < 0 || index >= (int)m_items.size())
        return 0;
    int m = (m_orientation == Horizontal)
        ? m_items[index].widget->minimumWidth()
        : m_items[index].widget->minimumHeight();
    if (!m_items[index].collapsible && m <= 0)
        m = 20;
    return std::max(0, m);
}

void JZSplitter::relayout()
{
    int n = (int)m_items.size();
    if (n == 0)
        return;

    int total = (m_orientation == Horizontal) ? width() : height();
    int handles = m_handleWidth * (n - 1);
    int avail = total - handles;
    if (avail < 0)
        avail = 0;

    int sumSizes = 0;
    for (int s : m_sizes)
        sumSizes += s;
    bool initialized = ((int)m_sizes.size() == n) && (sumSizes > 0);

    if (!initialized) {
        // 按拉伸因子初次分配
        int ts = totalStretch();
        if (ts <= 0)
            ts = n;
        m_sizes.assign(n, 0);
        int used = 0;
        for (int i = 0; i < n; ++i) {
            int s = (int)(avail * (int64_t)std::max(0, m_items[i].stretch) / ts);
            m_sizes[i] = s;
            used += s;
        }
        m_sizes[n - 1] += avail - used;          // 取整余量补到最后一项
        if (m_sizes[n - 1] < 0)
            m_sizes[n - 1] = 0;
    } else {
        // 已分配: 容器尺寸变化时按比例缩放, 并夹到各控件最小尺寸
        if (sumSizes != avail) {
            for (int i = 0; i < n; ++i)
                m_sizes[i] = std::max(m_sizes[i], minOf(i));
            int clampedSum = 0;
            for (int s : m_sizes)
                clampedSum += s;
            if (clampedSum > avail && clampedSum > 0) {
                int64_t scale = (int64_t)avail * 1000 / clampedSum;
                int newSum = 0;
                for (int i = 0; i < n; ++i) {
                    m_sizes[i] = (int)(m_sizes[i] * scale / 1000);
                    newSum += m_sizes[i];
                }
                m_sizes[n - 1] += avail - newSum;   // 余量补到最后
            } else if (clampedSum < avail) {
                m_sizes[n - 1] += avail - clampedSum;
            }
        }
    }

    // 摆放子控件
    int pos = 0;
    for (int i = 0; i < n; ++i) {
        int s = m_sizes[i];
        if (m_orientation == Horizontal)
            m_items[i].widget->setGeometry(pos, 0, s, height());
        else
            m_items[i].widget->setGeometry(0, pos, width(), s);
        pos += s + m_handleWidth;
    }
}

// ==================== 命中: 哪条分隔条 ====================
int JZSplitter::handleIndexAt(const JZPoint& p) const
{
    int n = (int)m_items.size();
    if (n < 2)
        return -1;
    int tol = 3;   // 抓取容差: 分隔条两侧各放 3px, 更易拖
    int x = (m_orientation == Horizontal) ? p.x() : p.y();
    int cum = 0;
    for (int i = 0; i < n - 1; ++i) {
        cum += m_sizes[i];                 // 第 i 项结束 = 第 i 条 handle 起点
        if (x >= cum - tol && x <= cum + m_handleWidth + tol)
            return i;
        cum += m_handleWidth;              // 越过 handle 到下一顶
    }
    return -1;
}

// ==================== 绘制 ====================
void JZSplitter::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width(), h = height();

    // 面板底色 (覆盖分隔条间隙与潜在边距)
    p.setBrush(JZBrush(JZColor(0xf2, 0xf2, 0xf2)));
    p.setPen(JZPen(JZColor(0xe2, 0xe2, 0xe2), 1));
    p.drawRect(0, 0, w, h);

    int n = (int)m_items.size();
    int cum = 0;
    for (int i = 0; i < n - 1; ++i) {
        cum += m_sizes[i];
        bool hot = (i == m_hoverHandle) || (i == m_dragHandle);
        JZBrush hb(hot ? JZColor(0xcf, 0xd8, 0xe8) : JZColor(0xd6, 0xd6, 0xd6));
        JZPen hp(JZColor(0xb6, 0xb6, 0xb6), 1);

        if (m_orientation == Horizontal) {
            int hx = cum;
            p.setBrush(hb); p.setPen(hp);
            p.drawRect(hx, 0, m_handleWidth, h);
            // 竖向抓握: 两条短竖线
            int cx = hx + m_handleWidth / 2;
            int gy0 = h / 2 - 8, gy1 = h / 2 + 8;
            p.setPen(JZPen(JZColor(0x88, 0x88, 0x88), 1));
            p.drawLine(cx - 2, gy0, cx - 2, gy1);
            p.drawLine(cx + 2, gy0, cx + 2, gy1);
        } else {
            int hy = cum;
            p.setBrush(hb); p.setPen(hp);
            p.drawRect(0, hy, w, m_handleWidth);
            // 横向抓握: 两条短横线
            int cy = hy + m_handleWidth / 2;
            int gx0 = w / 2 - 8, gx1 = w / 2 + 8;
            p.setPen(JZPen(JZColor(0x88, 0x88, 0x88), 1));
            p.drawLine(gx0, cy - 2, gx1, cy - 2);
            p.drawLine(gx0, cy + 2, gx1, cy + 2);
        }
        cum += m_handleWidth;
    }
}

// ==================== 鼠标交互 ====================
void JZSplitter::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() != JZMouseEvent::LeftButton) {
        JZWidget::mousePressEvent(event);
        return;
    }
    int h = handleIndexAt(event->localPos());
    if (h >= 0) {
        m_dragHandle = h;
        m_dragStart = (m_orientation == Horizontal)
            ? event->localPos().x() : event->localPos().y();
        m_dragSizes = m_sizes;
        grabMouse();   // 抓取: 拖出分隔条命中带也不丢 move/up
        update();
    } else {
        JZWidget::mousePressEvent(event);
    }
}

void JZSplitter::mouseMoveEvent(JZMouseEvent* event)
{
    if (m_dragHandle >= 0) {
        int cur = (m_orientation == Horizontal)
            ? event->localPos().x() : event->localPos().y();
        int delta = cur - m_dragStart;
        int i = m_dragHandle;
        int total = m_dragSizes[i] + m_dragSizes[i + 1];   // 两邻项总和保持不变
        int ni = m_dragSizes[i] + delta;
        int nj = total - ni;

        int mini = minOf(i), minj = minOf(i + 1);
        ni = std::max(mini, std::min(ni, total - minj));
        nj = total - ni;

        m_sizes[i] = ni;
        m_sizes[i + 1] = nj;
        relayout();
        update();
        sigSplitterMoved.emit(ni, i);
    } else {
        int h = handleIndexAt(event->localPos());
        if (h != m_hoverHandle) {
            m_hoverHandle = h;
            update();
        }
    }
}

void JZSplitter::mouseReleaseEvent(JZMouseEvent* event)
{
    if (m_dragHandle >= 0) {
        m_dragHandle = -1;
        releaseMouse();   // 释放抓取
        update();
    } else {
        JZWidget::mouseReleaseEvent(event);
    }
}
