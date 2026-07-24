#include "JZArcWidget.h"

JZ_OBJECT_IMPL(JZArcWidget, JZWidget)

JZArcWidget::JZArcWidget(JZWidget* parent)
    : JZWidget(parent)
    , sigValueChanged(this)
{
}

void JZArcWidget::setRange(int32_t min, int32_t max)
{}

void JZArcWidget::setValue(int32_t v)
{}

int32_t JZArcWidget::value() const
{ return 0; }

void JZArcWidget::setAngles(uint16_t bgStart, uint16_t bgEnd)
{}

void JZArcWidget::setRotation(uint16_t rot)
{}
