#include "JZPushButton.h"
#include "JZPainter.h"

JZ_OBJECT_IMPL(JZPushButton, JZWidget)

JZPushButton::JZPushButton(JZWidget* parent)
    : JZWidget(parent)
    , sigClicked(this)
{}

JZPushButton::JZPushButton(JZWidget* parent, int w, int h, const std::string& text)
    : JZPushButton(parent)
{
    resize(w, h);
    setText(text);
}

void* JZPushButton::labelHandle() const
{
    return m_label;
}

void JZPushButton::setText(const std::string& text)
{
    m_text = text;
}

std::string JZPushButton::text() const
{ return m_text; }

void JZPushButton::setWidth(int w)
{
    resize(w, height());
}

void JZPushButton::setHeight(int h)
{
    resize(width(), h);
}

JZSize JZPushButton::sizeHint() const
{
    int w = m_width > 0 ? m_width : 90;
    int h = m_height > 0 ? m_height : 30;
    return JZSize(w, h);
}

void JZPushButton::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width(), h = height();

    // 按下时底色加深, 给出按压反馈
    JZColor bg = m_pressed ? JZColor(0xc0, 0xc0, 0xc0) : JZColor(0xe0, 0xe0, 0xe0);
    p.setBrush(JZBrush(bg));
    p.setPen(JZPen(JZColor(0x80, 0x80, 0x80), 1));
    p.drawRoundRect(0, 0, w, h, 4);

    p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
    p.setFont(JZFont(14));
    if (!m_text.empty()) {
        int tw = (int)m_text.size() * 7;
        int tx = (w - tw) / 2;
        int ty = (h - 14) / 2;
        p.drawText(tx, ty, m_text.c_str());
    }
}

void JZPushButton::mousePressEvent(JZMouseEvent* event)
{
    m_pressed = true;
    update();
}

void JZPushButton::mouseReleaseEvent(JZMouseEvent* event)
{
    if (m_pressed)
        sigClicked.emit();
    m_pressed = false;
    update();
}
