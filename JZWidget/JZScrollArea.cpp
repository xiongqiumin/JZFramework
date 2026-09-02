#include "JZScrollArea.h"
#include "JZPainter.h"

#include <algorithm>


JZ_OBJECT_IMPL(JZScrollArea, JZWidget)

// ============================================================================
// JZScrollArea: 自身即视口。内容尺寸 m_contentW/H 决定可滚动范围 (-1 = 跟随
// 视口, 该方向不滚动); m_scrollX/Y 为滚动偏移。子类 (虚拟列表/表格) 在
// paintEvent 中用 scrollX()/scrollY() + viewport 宽高自行绘制可见内容。
// ============================================================================
JZScrollArea::JZScrollArea(JZWidget* parent)
    : JZWidget(parent)
{}

JZScrollArea::~JZScrollArea()
{
}

void JZScrollArea::setScrollDirection(int dir)
{}

void JZScrollArea::setScrollBarPolicy(ScrollBarPolicy policy)
{}

void JZScrollArea::setVerticalScrollBarPolicy(ScrollBarPolicy policy)
{
    setScrollBarPolicy(policy);
}

void JZScrollArea::setHorizontalScrollBarPolicy(ScrollBarPolicy policy)
{
    setScrollBarPolicy(policy);
}

void JZScrollArea::setContentSize(int w, int h)
{
    m_contentW = (w < 0) ? -1 : w;
    m_contentH = (h < 0) ? -1 : h;
    clampScroll();
    update();
}

int JZScrollArea::contentWidth() const
{
    return (m_contentW < 0) ? viewportWidth() : m_contentW;
}

int JZScrollArea::contentHeight() const
{
    return (m_contentH < 0) ? viewportHeight() : m_contentH;
}

int JZScrollArea::scrollX() const
{
    return m_scrollX;
}

int JZScrollArea::scrollY() const
{
    return m_scrollY;
}

int JZScrollArea::viewportWidth() const
{
    return std::max(0, width());
}

int JZScrollArea::viewportHeight() const
{
    return std::max(0, height());
}

void JZScrollArea::scrollTo(int x, int y)
{
    m_scrollX = x;
    m_scrollY = y;
    clampScroll();
    update();
}

void JZScrollArea::scrollToX(int x)
{
    scrollTo(x, m_scrollY);
}

void JZScrollArea::scrollToY(int y)
{
    scrollTo(m_scrollX, y);
}

void JZScrollArea::scrollToTop()
{
    scrollTo(m_scrollX, 0);
}

void JZScrollArea::scrollToBottom()
{
    scrollTo(m_scrollX, contentHeight());
}

bool JZScrollArea::canScrollVertically() const
{
    return contentHeight() > viewportHeight();
}

void JZScrollArea::clampScroll()
{
    int maxX = std::max(0, contentWidth() - viewportWidth());
    int maxY = std::max(0, contentHeight() - viewportHeight());
    m_scrollX = std::max(0, std::min(m_scrollX, maxX));
    m_scrollY = std::max(0, std::min(m_scrollY, maxY));
}

JZSize JZScrollArea::sizeHint() const
{
    if (m_width > 0 && m_height > 0)
        return JZSize(m_width, m_height);
    return JZSize(120, 120);
}

void JZScrollArea::resizeEvent(JZResizeEvent* event)
{
    JZWidget::resizeEvent(event);
    clampScroll();   // 视口变化后滚动范围可能收缩
}

void JZScrollArea::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width(), h = height();
    p.setBrush(JZBrush(JZColor(0xf5, 0xf5, 0xf5)));
    p.setPen(JZPen(JZColor(0xdd, 0xdd, 0xdd), 1));
    p.drawRect(0, 0, w, h);
}

// 滚轮: 纵向滚动 (一格 40px); 不可滚动时交基类 (内容不超出视口的子类不受影响)
void JZScrollArea::wheelEvent(JZMouseEvent* event)
{
    if (canScrollVertically() && event->wheelDelta() != 0) {
        scrollToY(m_scrollY - event->wheelDelta() * 40);
        return;
    }
    JZWidget::wheelEvent(event);
}
