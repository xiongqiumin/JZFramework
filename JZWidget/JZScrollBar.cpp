#include "JZScrollBar.h"
#include "JZPainter.h"
#include "JZFontMetrics.h"

#include <algorithm>

JZ_OBJECT_IMPL(JZScrollBar, JZWidget)

namespace {
// 画一个朝向 dir 的小三角箭头 (dir<0 左/上, dir>0 右/下)
void drawArrowTri(JZPainter& p, int cx, int cy, bool horiz, int dir)
{
    int s = 4;
    int x0, y0, x1, y1, x2, y2;
    if (horiz) {
        if (dir < 0) { x0 = cx - s; y0 = cy; x1 = cx + s; y1 = cy - s; x2 = cx + s; y2 = cy + s; }
        else         { x0 = cx + s; y0 = cy; x1 = cx - s; y1 = cy - s; x2 = cx - s; y2 = cy + s; }
    } else {
        if (dir < 0) { x0 = cx; y0 = cy - s; x1 = cx - s; y1 = cy + s; x2 = cx + s; y2 = cy + s; }
        else         { x0 = cx; y0 = cy + s; x1 = cx - s; y1 = cy - s; x2 = cx + s; y2 = cy - s; }
    }
    p.drawLine(x0, y0, x1, y1);
    p.drawLine(x1, y1, x2, y2);
    p.drawLine(x2, y2, x0, y0);
}
} // namespace

JZScrollBar::JZScrollBar(Orientation o, JZWidget* parent)
    : JZWidget(parent)
    , sigValueChanged(this)
{
    m_orient = o;
}

JZSize JZScrollBar::sizeHint() const
{
    int w = m_width > 0 ? m_width : (m_orient == Horizontal ? 120 : 16);
    int h = m_height > 0 ? m_height : (m_orient == Horizontal ? 16 : 120);
    return JZSize(w, h);
}

void JZScrollBar::setOrientation(Orientation o)
{
    if (m_orient == o) return;
    m_orient = o;
    update();
}

JZScrollBar::Orientation JZScrollBar::orientation() const { return m_orient; }

void JZScrollBar::setRange(int min, int max)
{
    m_min = min; m_max = max;
    setValue(m_value);
}

void JZScrollBar::setMinimum(int min) { setRange(min, m_max); }
int  JZScrollBar::minimum() const { return m_min; }
void JZScrollBar::setMaximum(int max) { setRange(m_min, max); }
int  JZScrollBar::maximum() const { return m_max; }

void JZScrollBar::setValue(int v)
{
    int nv = std::max(m_min, std::min(v, m_max));
    if (nv == m_value) return;
    m_value = nv;
    sigValueChanged.emit(m_value);
    update();
}

int JZScrollBar::value() const { return m_value; }

void JZScrollBar::setSingleStep(int step) { m_singleStep = std::max(1, step); }
int  JZScrollBar::singleStep() const { return m_singleStep; }
void JZScrollBar::setPageStep(int step) { m_pageStep = std::max(1, step); }
int  JZScrollBar::pageStep() const { return m_pageStep; }

// ==================== 几何 ====================
int JZScrollBar::grooveStart() const { return m_arrow; }

int JZScrollBar::grooveLen() const
{
    int mainLen = (m_orient == Horizontal) ? width() : height();
    return std::max(0, mainLen - 2 * m_arrow);
}

int JZScrollBar::thumbLen() const
{
    int gl = grooveLen();
    int r = range();
    if (r <= 0) return gl;
    int tl = (int)std::lround((double)gl * m_pageStep / (r + m_pageStep));
    return std::max(m_thumbMin, std::min(tl, gl));
}

int JZScrollBar::valueToPos(int value) const
{
    int gl = grooveLen();
    int tl = thumbLen();
    int r = range();
    if (r <= 0) return grooveStart();
    return grooveStart() + (int)std::lround((double)(value - m_min) / r * (gl - tl));
}

int JZScrollBar::posToValue(int pos) const
{
    int gl = grooveLen();
    int tl = thumbLen();
    int r = range();
    if (r <= 0 || (gl - tl) <= 0) return m_min;
    double ratio = (double)(pos - grooveStart()) / (gl - tl);
    return std::max(m_min, std::min(m_min + (int)std::lround(ratio * r), m_max));
}

int JZScrollBar::hitTest(const JZPoint& p) const
{
    int mainPos = (m_orient == Horizontal) ? p.x() : p.y();
    int mainLen = (m_orient == Horizontal) ? width() : height();
    if (mainPos < m_arrow) return -1;
    if (mainPos >= mainLen - m_arrow) return 3;
    int tp = valueToPos(m_value);
    int tl = thumbLen();
    if (mainPos >= tp && mainPos < tp + tl) return 1;
    return (mainPos < tp) ? 0 : 2;
}

// ==================== 绘制 ====================
void JZScrollBar::paintEvent(JZPainterEvent*)
{
    JZPainter p(this);
    int w = width(), h = height();
    bool horiz = (m_orient == Horizontal);
    int mainLen = horiz ? w : h;
    int crossLen = horiz ? h : w;

    // 凹槽背景
    int trackTh = 6;
    int tStart = grooveStart();
    int tLen = grooveLen();
    p.setBrush(JZBrush(JZColor(0xe8, 0xe8, 0xe8)));
    p.setPen(JZPen(JZColor(0xd0, 0xd0, 0xd0), 1));
    if (horiz) p.drawRoundRect(tStart, (h - trackTh) / 2, tLen, trackTh, trackTh / 2);
    else       p.drawRoundRect((w - trackTh) / 2, tStart, trackTh, tLen, trackTh / 2);

    // 滑块
    int tp = valueToPos(m_value);
    int tl = thumbLen();
    bool thumbPressed = (m_pressCode == 1);
    p.setBrush(JZBrush(thumbPressed ? JZColor(0xa8, 0xa8, 0xb2) : JZColor(0xc0, 0xc0, 0xc8)));
    p.setPen(JZPen(JZColor(0x98, 0x98, 0xa0), 1));
    if (horiz) p.drawRoundRect(tp, 2, tl, h - 4, 3);
    else       p.drawRoundRect(2, tp, w - 4, tl, 3);

    // 两端箭头按钮
    int cx0 = m_arrow / 2, cx1 = mainLen - m_arrow / 2;
    int cyc = crossLen / 2;
    if (horiz) {
        if (m_pressCode == -1) { p.setBrush(JZBrush(JZColor(0xd8, 0xd8, 0xd8))); p.setPen(JZPen(JZColor(0xb0,0xb0,0xb0),1)); p.drawRect(0, 0, m_arrow, h); }
        if (m_pressCode == 3)  { p.setBrush(JZBrush(JZColor(0xd8, 0xd8, 0xd8))); p.setPen(JZPen(JZColor(0xb0,0xb0,0xb0),1)); p.drawRect(mainLen - m_arrow, 0, m_arrow, h); }
        p.setPen(JZPen(JZColor(0x60, 0x60, 0x60), 1));
        drawArrowTri(p, cx0, cyc, true, -1);
        drawArrowTri(p, cx1, cyc, true, +1);
    } else {
        if (m_pressCode == -1) { p.setBrush(JZBrush(JZColor(0xd8, 0xd8, 0xd8))); p.setPen(JZPen(JZColor(0xb0,0xb0,0xb0),1)); p.drawRect(0, 0, w, m_arrow); }
        if (m_pressCode == 3)  { p.setBrush(JZBrush(JZColor(0xd8, 0xd8, 0xd8))); p.setPen(JZPen(JZColor(0xb0,0xb0,0xb0),1)); p.drawRect(0, mainLen - m_arrow, w, m_arrow); }
        p.setPen(JZPen(JZColor(0x60, 0x60, 0x60), 1));
        drawArrowTri(p, cyc, cx0, false, -1);
        drawArrowTri(p, cyc, cx1, false, +1);
    }
}

// ==================== 交互 ====================
void JZScrollBar::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() != JZMouseEvent::LeftButton) {
        JZWidget::mousePressEvent(event);
        return;
    }
    int code = hitTest(event->localPos());
    m_pressCode = code;
    m_dragGrab = 0;
    if (code == -1)      setValue(m_value - m_singleStep);
    else if (code == 3)  setValue(m_value + m_singleStep);
    else if (code == 1) {
        m_dragging = true;
        m_dragGrab = (m_orient == Horizontal ? event->localPos().x() : event->localPos().y())
                     - valueToPos(m_value);
    }
    else if (code == 0)  setValue(m_value - m_pageStep);
    else if (code == 2)  setValue(m_value + m_pageStep);
    update();
}

void JZScrollBar::mouseMoveEvent(JZMouseEvent* event)
{
    if (!m_dragging) return;
    int mp = (m_orient == Horizontal ? event->localPos().x() : event->localPos().y());
    setValue(posToValue(mp - m_dragGrab));
}

void JZScrollBar::mouseReleaseEvent(JZMouseEvent* event)
{
    m_dragging = false;
    m_pressCode = -2;
    update();
}

void JZScrollBar::wheelEvent(JZMouseEvent* event)
{
    if (event->wheelDelta() != 0) {
        setValue(m_value + (event->wheelDelta() > 0 ? m_singleStep : -m_singleStep));
        return;
    }
    JZWidget::wheelEvent(event);
}
