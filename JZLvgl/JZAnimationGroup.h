#ifndef JZ_ANIMATION_GROUP_H_
#define JZ_ANIMATION_GROUP_H_

#include "JZSignal.h"
#include "JZObject.h"
#include "JZPropertyAnimation.h"
#include <vector>

class JZLvglWidget;

class JZAnimationGroup : public JZObject
{
public:
    JZSigVoid sigFinished;

    JZAnimationGroup(JZObject* parent = nullptr);
    ~JZAnimationGroup();

    void setDuration(uint32_t ms);
    void setDelay(uint32_t ms);

    void addProperty(JZLvglWidget* target, const std::string& name,
                     int32_t from, int32_t to, uint32_t startTime = 0);
    void addAnimation(JZAbstractAnimation* anim, uint32_t startTime = 0);

    void start();
    void stop();

private:
    uint32_t m_duration = 300;
    uint32_t m_delay = 0;
    std::vector<JZAbstractAnimation*> m_anims;
    int m_doneCount = 0;
};

#endif
