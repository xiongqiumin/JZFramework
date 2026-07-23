#include "JZTextEdit.h"

JZ_OBJECT_IMPL(JZTextEdit, JZWidget)

JZTextEdit::JZTextEdit(JZWidget* parent)
    : JZWidget(lv_textarea_create, parent)
{
}

void JZTextEdit::setText(const std::string& text)
{
    lv_textarea_set_text(m_lv, text.c_str());
}

std::string JZTextEdit::text() const
{
    return lv_textarea_get_text(m_lv);
}

void JZTextEdit::setPassword(bool pw)
{
    lv_textarea_set_password_mode(m_lv, pw);
}
