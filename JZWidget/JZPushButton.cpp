#include "JZPushButton.h"

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
{}

std::string JZPushButton::text() const
{ return {}; }

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
