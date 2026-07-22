#include "JZRadioButton.h"
#include "lvgl.h"
#include "JZPainter.h"
#include "JZPen.h"
#include "JZBrush.h"
#include "JZColor.h"

JZ_OBJECT_IMPL(JZRadioButton, JZWidget)

JZRadioButton::JZRadioButton(JZWidget* parent)
    : JZWidget(lv_obj_create, parent)
    , sigToggled(this)
{
    // 容器本体透明, 用 flex 横向排列 [圆环占位 + 文本]
    lv_obj_set_style_bg_opa(m_lv, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m_lv, 0, 0);
    lv_obj_set_style_pad_gap(m_lv, 6, 0);
    lv_obj_set_flex_flow(m_lv, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(m_lv, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(m_lv, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    // 18x18 透明占位, 仅为圆环预留布局宽度(圆环在 paintEvent 中绘制)
    m_indicator = lv_obj_create(m_lv);
    lv_obj_set_size(m_indicator, 18, 18);
    lv_obj_set_style_bg_opa(m_indicator, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m_indicator, 0, 0);

    // 文本
    m_label = lv_label_create(m_lv);
    lv_label_set_text(m_label, "");
}

JZRadioButton::JZRadioButton(JZWidget* parent, const std::string& text)
    : JZRadioButton(parent)
{
    setText(text);
}

void JZRadioButton::setText(const std::string& text)
{
    lv_label_set_text(m_label, text.c_str());
}

std::string JZRadioButton::text() const
{
    return lv_label_get_text(m_label);
}

bool JZRadioButton::isChecked() const
{
    return m_checked;
}

void JZRadioButton::setChecked(bool on)
{
    if (m_checked == on) return;
    m_checked = on;
    update();
}

void JZRadioButton::paintEvent(JZPainterEvent*)
{
    JZPainter p(this);
    int d = 18;
    int y = (height() - d) / 2;
    int cx = d / 2;
    int cy = y + d / 2;

    // 外环(圆)
    JZPen ringPen(JZColor(0x33, 0x66, 0xcc), 2);
    p.setPen(ringPen);
    p.drawRoundRect(1, y + 1, d - 2, d - 2, (d - 2) / 2);

    // 选中: 内圆点
    if (m_checked) {
        int dd = 8;
        p.fillRect(cx - dd / 2, cy - dd / 2, dd, dd, JZColor(0x33, 0x66, 0xcc));
    }
}

void JZRadioButton::onLvEvent(lv_event_t* e)
{
    JZWidget::onLvEvent(e);
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (m_checked)
            return;   // 已选中, radio 不再反选

        // 取消同 parentWidget 下其他 JZRadioButton 的选中
        auto* pw = parentWidget();
        if (pw) {
            for (auto* child : pw->children()) {
                auto* rb = jzobject_cast<JZRadioButton*>(child);
                if (rb && rb != this && rb->isChecked())
                    rb->setChecked(false);
            }
        }
        setChecked(true);
        sigToggled.emit();
    }
}
