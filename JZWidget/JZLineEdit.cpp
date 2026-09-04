#include "JZLineEdit.h"

JZ_OBJECT_IMPL(JZLineEdit, JZAbstractTextEdit)

JZLineEdit::JZLineEdit(JZWidget* parent)
    : JZAbstractTextEdit(parent, true)
{
}

JZSize JZLineEdit::sizeHint() const
{
    return JZSize(160, 28);
}

void JZLineEdit::setPassword(bool password)
{
    setPasswordMode(password);
}

bool JZLineEdit::isPassword() const
{
    return isPasswordMode();
}

void JZLineEdit::setMaxLength(int n)
{
    setMaxLengthLimit(n);
}

int JZLineEdit::maxLength() const
{
    return maxLengthLimit();
}
