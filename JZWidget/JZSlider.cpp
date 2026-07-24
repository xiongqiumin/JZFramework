#include "JZSlider.h"

JZ_OBJECT_IMPL(JZSlider, JZWidget)

JZSlider::JZSlider(JZWidget* parent)
    : JZWidget(parent)
    , sigValueChanged(this)
{
}

void JZSlider::setRange(int32_t min, int32_t max)
{}

void JZSlider::setValue(int32_t v)
{}

int32_t JZSlider::value() const
{ return 0; }

void JZSlider::onLvEvent(void* e)
{}
