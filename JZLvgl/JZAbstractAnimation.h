#ifndef JZ_ABSTRACT_ANIMATION_H_
#define JZ_ABSTRACT_ANIMATION_H_

#include "lvgl.h"
#include "JZSignal.h"
#include "JZObject.h"

class JZLvglWidget;

class JZAbstractAnimation : public JZObject
{
public:
    JZSigVoid sigFinished;

    JZAbstractAnimation(JZObject* parent = nullptr);
    ~JZAbstractAnimation();

    void setTarget(lv_obj_t* obj)         { lv_anim_set_var(&m_anim, obj); m_target = obj; }
    void setTarget(JZLvglWidget* widget);

    void setDuration(uint32_t ms)         { lv_anim_set_duration(&m_anim, ms); }
    void setDelay(uint32_t ms)            { lv_anim_set_delay(&m_anim, ms); }

    void setEasingLinear()                { lv_anim_set_path_cb(&m_anim, lv_anim_path_linear); }
    void setEasingEaseIn()                { lv_anim_set_path_cb(&m_anim, lv_anim_path_ease_in); }
    void setEasingEaseOut()               { lv_anim_set_path_cb(&m_anim, lv_anim_path_ease_out); }
    void setEasingEaseInOut()             { lv_anim_set_path_cb(&m_anim, lv_anim_path_ease_in_out); }
    void setEasingOvershoot()             { lv_anim_set_path_cb(&m_anim, lv_anim_path_overshoot); }
    void setEasingBounce()                { lv_anim_set_path_cb(&m_anim, lv_anim_path_bounce); }

    void setExecCb(lv_anim_exec_xcb_t cb) { lv_anim_set_exec_cb(&m_anim, cb); }

    void start();
    void stop();

    lv_anim_t m_anim;
    lv_obj_t* m_target = nullptr;

private:
    static void s_readyCb(lv_anim_t* a);
};

#endif
