#ifndef JZ_PROPERTY_ANIMATION_H_
#define JZ_PROPERTY_ANIMATION_H_

#include "JZAbstractAnimation.h"
#include <string>

class JZPropertyAnimation : public JZAbstractAnimation
{
public:
    JZPropertyAnimation(JZObject* parent = nullptr)
        : JZAbstractAnimation(parent) {}

    // 属性名 + 起止值 → 回调中调 widget->setProperty(name, value)
    //bool setProperty(const std::string& name, int32_t start, int32_t end);

    std::string m_propName;
};

#endif
