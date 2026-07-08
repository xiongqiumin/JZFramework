#include "JZPushButton.h"

JZ_OBJECT_IMPL(JZPushButton, JZWidget)

JZPushButton::JZPushButton(JZWidget* parent)
    : JZWidget(lv_button_create, parent)
    , sigClicked(this)
{
    m_label = lv_label_create(m_lv);
    lv_label_set_text(m_label, "");
    lv_obj_center(m_label);
}

JZPushButton::JZPushButton(JZWidget* parent, int w, int h, const std::string& text)
    : JZPushButton(parent)
{
    resize(w, h);
    setText(text);
}

lv_obj_t* JZPushButton::labelHandle() const
{
    return m_label;
}

void JZPushButton::onLvEvent(lv_event_t* e)
{
    JZWidget::onLvEvent(e);

    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        sigClicked.emit();
    }
}

void JZPushButton::setText(const std::string& text)
{
    lv_label_set_text(m_label, text.c_str());
}

std::string JZPushButton::text() const
{
    return lv_label_get_text(m_label);
}

void JZPushButton::setWidth(int w)
{
    resize(w, height());
}

void JZPushButton::setHeight(int h)
{
    resize(width(), h);
}
