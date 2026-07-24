#include "JZSwitch.h"

JZ_OBJECT_IMPL(JZSwitch, JZWidget)

JZSwitch::JZSwitch(JZWidget* parent)
    : JZWidget(parent)
    , sigToggled(this)
{
}

void JZSwitch::setState(bool on)
{}

bool JZSwitch::isOn() const
{ return false; }