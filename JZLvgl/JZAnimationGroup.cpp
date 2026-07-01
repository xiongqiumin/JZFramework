#include "JZAnimationGroup.h"
#include "JZLvglWidget.h"

JZAnimationGroup::JZAnimationGroup(JZObject* parent)
    : JZObject(parent)
    , sigFinished(this)
{
}

JZAnimationGroup::~JZAnimationGroup()
{
    stop();
}

void JZAnimationGroup::setDuration(uint32_t ms)
{
    m_duration = ms;
}

void JZAnimationGroup::setDelay(uint32_t ms)
{
    m_delay = ms;
}

void JZAnimationGroup::addProperty(JZLvglWidget* target, const std::string& name,
                                    int32_t from, int32_t to, uint32_t startTime)
{
    auto* anim = new JZPropertyAnimation();
    anim->setTarget(target);
    anim->setDuration(m_duration);
    anim->setDelay(m_delay);
    anim->setEasingEaseInOut();
    anim->setProperty(name, from, to);
    addAnimation(anim, (int)startTime);
}

void JZAnimationGroup::addAnimation(JZAbstractAnimation* anim, uint32_t /*startTime*/)
{
    connect(&anim->sigFinished, [this]() {
        m_doneCount++;
        if (m_doneCount >= (int)m_anims.size())
            sigFinished.emit();
    });
    m_anims.push_back(anim);
}

void JZAnimationGroup::start()
{
    if (m_anims.empty()) return;
    m_doneCount = 0;

    for (auto* anim : m_anims)
        anim->start();
}

void JZAnimationGroup::stop()
{
    for (auto* a : m_anims) delete a;
    m_anims.clear();
    m_doneCount = 0;
}
