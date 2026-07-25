#include "JZScrollArea.h"
#include "JZPainter.h"


JZ_OBJECT_IMPL(JZScrollArea, JZWidget)

// ============================================================================
// JZScrollArea
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
{}

int JZScrollArea::contentWidth() const
{ return 0; }

int JZScrollArea::contentHeight() const
{ return 0; }

int JZScrollArea::scrollX() const
{ return 0; }

int JZScrollArea::scrollY() const
{ return 0; }

int JZScrollArea::viewportWidth() const
{ return 0; }

int JZScrollArea::viewportHeight() const
{ return 0; }

void JZScrollArea::scrollTo(int x, int y)
{}

void JZScrollArea::scrollToX(int x)
{}

void JZScrollArea::scrollToY(int y)
{}

void JZScrollArea::scrollToTop()
{}

void JZScrollArea::scrollToBottom()
{}

bool JZScrollArea::canScrollVertically() const
{
    return contentHeight() > viewportHeight();
}

JZSize JZScrollArea::sizeHint() const
{
    if (m_width > 0 && m_height > 0)
        return JZSize(m_width, m_height);
    return JZSize(120, 120);
}

void JZScrollArea::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width(), h = height();
    p.setBrush(JZBrush(JZColor(0xf5, 0xf5, 0xf5)));
    p.setPen(JZPen(JZColor(0xdd, 0xdd, 0xdd), 1));
    p.drawRect(0, 0, w, h);
}
