#include "JZLabel.h"

JZ_OBJECT_IMPL(JZLabel, JZWidget)

JZLabel::JZLabel(JZWidget* parent)
    : JZWidget(parent)
{
}

JZLabel::JZLabel(JZWidget* parent, const std::string& text)
    : JZLabel(parent)
{
    setText(text);
}

void JZLabel::setText(const std::string& text)
{
}

std::string JZLabel::text() const
{ 
    return {}; 
}

void JZLabel::setAlign(int align)
{
}

JZSize JZLabel::sizeHint() const
{
    return JZSize(80, 20);
}
