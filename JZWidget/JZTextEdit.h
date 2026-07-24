#ifndef JZ_WIDGET_TEXTAREA_H_
#define JZ_WIDGET_TEXTAREA_H_

#include "JZWidget.h"

class JZTextEdit : public JZWidget
{
    JZ_OBJECT_DECLARE(JZTextEdit)

public:
    explicit JZTextEdit(JZWidget* parent = nullptr);

    void setText(const std::string& text);
    std::string text() const;

    void setPassword(bool pw);
};

#endif
