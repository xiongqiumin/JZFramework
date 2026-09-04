#include "JZTextEdit.h"

JZ_OBJECT_IMPL(JZTextEdit, JZAbstractTextEdit)

JZTextEdit::JZTextEdit(JZWidget* parent)
    : JZAbstractTextEdit(parent, false)
{
}

JZSize JZTextEdit::sizeHint() const
{
    return JZSize(200, 70);
}

void JZTextEdit::setLineWrap(bool wrap)
{
    setLineWrapMode(wrap);
}

bool JZTextEdit::lineWrap() const
{
    return isLineWrapMode();
}
