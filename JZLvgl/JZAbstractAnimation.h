#ifndef JZ_ABSTRACT_ANIMATION_H_
#define JZ_ABSTRACT_ANIMATION_H_

#include "lvgl.h"
#include "JZSignal.h"
#include "JZObject.h"

class JZWidget;

class JZAbstractAnimation : public JZObject
{
    JZ_OBJECT_DECLARE(JZAbstractAnimation)

public:
    JZAbstractAnimation(JZObject* parent = nullptr);
    ~JZAbstractAnimation();

    void setTarget(lv_obj_t* obj);
    void setTarget(JZWidget* widget);

    void setDuration(uint32_t ms);
    void setDelay(uint32_t ms);

    void setEasingLinear();
    void setEasingEaseIn();
    void setEasingEaseOut();
    void setEasingEaseInOut();
    void setEasingOvershoot();
    void setEasingBounce();

    void setExecCb(lv_anim_exec_xcb_t cb);

    void start();
    void stop();

jz_signals:
    JZSigVoid sigFinished;

    lv_anim_t m_anim;
    lv_obj_t* m_target = nullptr;

private:
    static void s_readyCb(lv_anim_t* a);
};

#endif
