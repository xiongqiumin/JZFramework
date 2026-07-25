#include "JZProgressBar.h"

JZ_OBJECT_IMPL(JZProgressBar, JZWidget)

JZProgressBar::JZProgressBar(JZWidget* parent)
    : JZWidget(parent)
{
}

void JZProgressBar::setRange(int32_t min, int32_t max)
{}

void JZProgressBar::setValue(int32_t v, bool anim)
{}

int32_t JZProgressBar::value() const
{ return 0; }

JZSize JZProgressBar::sizeHint() const
{
    int w = m_width > 0 ? m_width : 120;
    return JZSize(w, 16);
}
