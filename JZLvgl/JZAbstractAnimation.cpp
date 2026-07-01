#include "JZAbstractAnimation.h"
#include "JZLvglWidget.h"

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

void JZAbstractAnimation::setTarget(JZLvglWidget* widget)
{
    m_target = widget->handle();
    lv_anim_set_var(&m_anim, m_target);
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
