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
