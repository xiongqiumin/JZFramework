#include "JZAbstractAnimation.h"
#include "JZWidget.h"

JZ_OBJECT_IMPL(JZAbstractAnimation, JZObject)

JZAbstractAnimation::JZAbstractAnimation(JZObject* parent)
    : JZObject(parent)
    , sigFinished(this)
{}

JZAbstractAnimation::~JZAbstractAnimation()
{
    stop();
}

void JZAbstractAnimation::setTarget(void* obj)
{}

void JZAbstractAnimation::setTarget(JZWidget* widget)
{}

void JZAbstractAnimation::setDuration(uint32_t ms)
{}

void JZAbstractAnimation::setDelay(uint32_t ms)
{}

void JZAbstractAnimation::setEasingLinear()
{}

void JZAbstractAnimation::setEasingEaseIn()
{}

void JZAbstractAnimation::setEasingEaseOut()
{}

void JZAbstractAnimation::setEasingEaseInOut()
{}

void JZAbstractAnimation::setEasingOvershoot()
{}

void JZAbstractAnimation::setEasingBounce()
{}

void JZAbstractAnimation::setExecCb(JZAnimExecCb cb)
{}

void JZAbstractAnimation::start()
{}

void JZAbstractAnimation::stop()
{}

void JZAbstractAnimation::s_readyCb(void* a)
{}
