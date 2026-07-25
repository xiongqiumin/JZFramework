#include "JZTextEdit.h"

JZ_OBJECT_IMPL(JZTextEdit, JZWidget)

JZTextEdit::JZTextEdit(JZWidget* parent)
    : JZWidget(parent)
{
}

void JZTextEdit::setText(const std::string& text)
{}

std::string JZTextEdit::text() const
{ return {}; }

void JZTextEdit::setPassword(bool pw)
{}

JZSize JZTextEdit::sizeHint() const
{
    int w = m_width > 0 ? m_width : 160;
    int h = m_height > 0 ? m_height : 70;
    return JZSize(w, h);
}
