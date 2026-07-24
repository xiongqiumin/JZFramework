#ifndef JZ_ABSTRACT_ANIMATION_H_
#define JZ_ABSTRACT_ANIMATION_H_

#include "JZSignal.h"
#include "JZObject.h"

typedef void (*JZAnimExecCb)(void*, int32_t);

class JZWidget;


class JZAbstractAnimation : public JZObject
{
    JZ_OBJECT_DECLARE(JZAbstractAnimation)

public:
    JZAbstractAnimation(JZObject* parent = nullptr);
    ~JZAbstractAnimation();

    void setTarget(void* obj);
    void setTarget(JZWidget* widget);

    void setDuration(uint32_t ms);
    void setDelay(uint32_t ms);

    void setEasingLinear();
    void setEasingEaseIn();
    void setEasingEaseOut();
    void setEasingEaseInOut();
    void setEasingOvershoot();
    void setEasingBounce();

    void setExecCb(JZAnimExecCb cb);

    void start();
    void stop();

jz_signals:
    JZSigVoid sigFinished;

    void* m_anim = nullptr;
    void* m_target = nullptr;

private:
    static void s_readyCb(void* a);
};

#endif
