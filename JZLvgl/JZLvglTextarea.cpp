#include "JZLvglTextarea.h"

JZLvglTextarea::JZLvglTextarea(JZLvglWidget* parent)
    : JZLvglWidget(lv_textarea_create, parent)
{
}

void JZLvglTextarea::setText(const std::string& text)
{
    lv_textarea_set_text(m_lv, text.c_str());
}

std::string JZLvglTextarea::text() const
{
    return lv_textarea_get_text(m_lv);
}
