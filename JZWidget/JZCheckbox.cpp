#include "JZCheckbox.h"

JZ_OBJECT_IMPL(JZCheckbox, JZWidget)

JZCheckbox::JZCheckbox(JZWidget* parent)
    : JZWidget(parent)
    , sigToggled(this)
{
}

void JZCheckbox::setText(const std::string& text)
{}

std::string JZCheckbox::text() const
{ return {}; }

bool JZCheckbox::isChecked() const
{ return false; }

JZSize JZCheckbox::sizeHint() const
{
    return JZSize(96, 24);
}
