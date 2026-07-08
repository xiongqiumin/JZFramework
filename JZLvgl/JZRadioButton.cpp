#include "JZRadioButton.h"

JZ_OBJECT_IMPL(JZRadioButton, JZWidget)

JZRadioButton::JZRadioButton(JZWidget* parent)
    : JZWidget(lv_checkbox_create, parent)
    , sigToggled(this)
{
}

JZRadioButton::JZRadioButton(JZWidget* parent, const std::string& text)
    : JZRadioButton(parent)
{
    setText(text);
}

void JZRadioButton::setText(const std::string& text)
{
    lv_checkbox_set_text(m_lv, text.c_str());
}

std::string JZRadioButton::text() const
{
    return lv_checkbox_get_text(m_lv);
}

bool JZRadioButton::isChecked() const
{
    return lv_obj_has_state(m_lv, LV_STATE_CHECKED);
}

void JZRadioButton::setChecked(bool on)
{
    if (on) {
        lv_obj_add_state(m_lv, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(m_lv, LV_STATE_CHECKED);
    }
}

void JZRadioButton::onLvEvent(lv_event_t* e)
{
    JZWidget::onLvEvent(e);
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        // 取消同 parentWidget 下其他 JZRadioButton 的选中
        auto* pw = parentWidget();
        if (pw) {
            for (auto* child : pw->children()) {
                auto* rb = jzobject_cast<JZRadioButton*>(child);
                if (rb && rb != this && rb->isChecked())
                    rb->setChecked(false);
            }
        }
        lv_obj_add_state(m_lv, LV_STATE_CHECKED);
        sigToggled.emit();
    }
}
