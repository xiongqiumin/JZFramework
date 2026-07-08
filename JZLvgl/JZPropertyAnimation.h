#ifndef JZ_PROPERTY_ANIMATION_H_
#define JZ_PROPERTY_ANIMATION_H_

#include "JZAbstractAnimation.h"
#include <string>

class JZPropertyAnimation : public JZAbstractAnimation
{
    JZ_OBJECT_DECLARE(JZPropertyAnimation)

public:
    JZPropertyAnimation(JZObject* parent = nullptr);

    std::string m_propName;
};

#endif
