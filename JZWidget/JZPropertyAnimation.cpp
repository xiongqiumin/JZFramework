#include "JZPropertyAnimation.h"
#include "JZWidget.h"

JZ_OBJECT_IMPL(JZPropertyAnimation, JZAbstractAnimation)

JZPropertyAnimation::JZPropertyAnimation(JZObject* parent)
    : JZAbstractAnimation(parent)
{
}

static void s_propExecCb(void* var, int32_t value)
{}
/*
bool JZPropertyAnimation::setProperty(const std::string& name, int32_t start, int32_t end)
{ return {}; }
*/