#include "JZLabel.h"

JZ_OBJECT_IMPL(JZLabel, JZWidget)

JZLabel::JZLabel(JZWidget* parent)
    : JZWidget(lv_label_create, parent)
{
}

JZLabel::JZLabel(JZWidget* parent, const std::string& text)
    : JZLabel(parent)
{
    setText(text);
}

void JZLabel::setText(const std::string& text)
{
    lv_label_set_text(m_lv, text.c_str());
}

std::string JZLabel::text() const
{
    return lv_label_get_text(m_lv);
}

void JZLabel::setAlign(lv_text_align_t align)
{
    lv_obj_set_style_text_align(m_lv, align, 0);
}

void JZLabel::setLongMode(lv_label_long_mode_t mode)
{
    lv_label_set_long_mode(m_lv, mode);
}
