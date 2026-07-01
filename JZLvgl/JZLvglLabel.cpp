#include "JZLvglLabel.h"

JZLvglLabel::JZLvglLabel(JZLvglWidget* parent)
    : JZLvglWidget(lv_label_create, parent)
{
}

JZLvglLabel::JZLvglLabel(JZLvglWidget* parent, const std::string& text)
    : JZLvglLabel(parent)
{
    setText(text);
}

void JZLvglLabel::setText(const std::string& text)
{
    lv_label_set_text(m_lv, text.c_str());
}

std::string JZLvglLabel::text() const
{
    return lv_label_get_text(m_lv);
}
