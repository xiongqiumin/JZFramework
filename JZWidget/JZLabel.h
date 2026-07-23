#ifndef JZ_WIDGET_LABEL_H_
#define JZ_WIDGET_LABEL_H_

#include "JZWidget.h"

class JZLabel : public JZWidget
{
    JZ_OBJECT_DECLARE(JZLabel)

public:
    explicit JZLabel(JZWidget* parent = nullptr);
    JZLabel(JZWidget* parent, const std::string& text);

    void setText(const std::string& text);
    std::string text() const;

    void setAlign(lv_text_align_t align);
    void setLongMode(lv_label_long_mode_t mode);
};

#endif
