#include "JZRoller.h"

JZ_OBJECT_IMPL(JZRoller, JZWidget)

JZRoller::JZRoller(JZWidget* parent)
    : JZWidget(parent)
{
}

void JZRoller::setOptions(const std::string& opts, int mode)
{}

void JZRoller::setVisibleRowCount(uint8_t n)
{}

void JZRoller::setSelected(uint32_t idx, bool anim)
{}

uint32_t JZRoller::selected() const
{ return 0; }
