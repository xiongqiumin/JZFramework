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

void JZPushButton::onLvEvent(void* e)
{}

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
