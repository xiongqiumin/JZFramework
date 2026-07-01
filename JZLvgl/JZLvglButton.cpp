#include "JZLvglButton.h"

JZLvglButton::JZLvglButton(JZLvglWidget* parent)
    : JZLvglWidget(lv_button_create, parent)
    , sigClicked(this)
{
    m_label = lv_label_create(m_lv);
    lv_label_set_text(m_label, "");
    lv_obj_center(m_label);
}

JZLvglButton::JZLvglButton(JZLvglWidget* parent, int w, int h, const std::string& text)
    : JZLvglButton(parent)
{
    setSize(w, h);
    setText(text);
}

void JZLvglButton::onLvEvent(lv_event_t* e)
{
    JZLvglWidget::onLvEvent(e);

    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        sigClicked.emit();
    }
}

void JZLvglButton::setText(const std::string& text)
{
    lv_label_set_text(m_label, text.c_str());
}

std::string JZLvglButton::text() const
{
    return lv_label_get_text(m_label);
}
