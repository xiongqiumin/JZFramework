#include "JZAbstractAnimation.h"
#include "JZWidget.h"

JZ_OBJECT_IMPL(JZAbstractAnimation, JZObject)

JZAbstractAnimation::JZAbstractAnimation(JZObject* parent)
    : JZObject(parent)
    , sigFinished(this)
{
    lv_anim_init(&m_anim);
    lv_anim_set_user_data(&m_anim, this);
}

JZAbstractAnimation::~JZAbstractAnimation()
{
    stop();
}

void JZAbstractAnimation::setTarget(lv_obj_t* obj)
{
    lv_anim_set_var(&m_anim, obj);
    m_target = obj;
}

void JZAbstractAnimation::setTarget(JZWidget* widget)
{
    m_target = widget->handle();
    lv_anim_set_var(&m_anim, m_target);
}

void JZAbstractAnimation::setDuration(uint32_t ms)
{
    lv_anim_set_duration(&m_anim, ms);
}

void JZAbstractAnimation::setDelay(uint32_t ms)
{
    lv_anim_set_delay(&m_anim, ms);
}

void JZAbstractAnimation::setEasingLinear()
{
    lv_anim_set_path_cb(&m_anim, lv_anim_path_linear);
}

void JZAbstractAnimation::setEasingEaseIn()
{
    lv_anim_set_path_cb(&m_anim, lv_anim_path_ease_in);
}

void JZAbstractAnimation::setEasingEaseOut()
{
    lv_anim_set_path_cb(&m_anim, lv_anim_path_ease_out);
}

void JZAbstractAnimation::setEasingEaseInOut()
{
    lv_anim_set_path_cb(&m_anim, lv_anim_path_ease_in_out);
}

void JZAbstractAnimation::setEasingOvershoot()
{
    lv_anim_set_path_cb(&m_anim, lv_anim_path_overshoot);
}

void JZAbstractAnimation::setEasingBounce()
{
    lv_anim_set_path_cb(&m_anim, lv_anim_path_bounce);
}

void JZAbstractAnimation::setExecCb(lv_anim_exec_xcb_t cb)
{
    lv_anim_set_exec_cb(&m_anim, cb);
}

void JZAbstractAnimation::start()
{
    lv_anim_set_completed_cb(&m_anim, s_readyCb);
    lv_anim_start(&m_anim);
}

void JZAbstractAnimation::stop()
{
    lv_anim_delete(m_target, nullptr);
    lv_anim_init(&m_anim);
    lv_anim_set_user_data(&m_anim, this);
}

void JZAbstractAnimation::s_readyCb(lv_anim_t* a)
{
    auto* anim = (JZAbstractAnimation*)lv_anim_get_user_data(a);
    if (anim) anim->sigFinished.emit();
}
