#include "JZPropertyAnimation.h"
#include "JZLvglWidget.h"

static void s_propExecCb(void* var, int32_t value)
{
    auto* lvObj = (lv_obj_t*)var;
    auto* widget = (JZLvglWidget*)lv_obj_get_user_data(lvObj);
    if (!widget) return;
    // 从当前运行的动画中找到对应的 anim
    lv_anim_t* a = lv_anim_get(var, s_propExecCb);
    if (a) {
        auto* anim = (JZPropertyAnimation*)lv_anim_get_user_data(a);
        if (anim) widget->setProperty(anim->m_propName, JZVariant((int)value));
    }
}

bool JZPropertyAnimation::setProperty(const std::string& name, int32_t start, int32_t end)
{
    m_propName = name;
    lv_anim_set_values(&m_anim, start, end);
    setExecCb(s_propExecCb);
    return true;
}
