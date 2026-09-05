#include "JZAnimationGroup.h"
#include "JZWidget.h"

JZ_OBJECT_IMPL(JZAnimationGroup, JZObject)

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

void JZAnimationGroup::addProperty(JZWidget* target, const std::string& name,
                                    int32_t from, int32_t to, uint32_t startTime)
{
    auto* anim = new JZPropertyAnimation();
    anim->setTarget(target);
    anim->setDuration(m_duration);
    anim->setDelay(m_delay + startTime);     // startTime = 与组起点错开的附加延迟
    anim->setEasingEaseInOut();
    if (!anim->setProperty(name, from, to))
    {
        delete anim;                         // 属性名不支持/目标非法: 不参与组
        return;
    }
    addAnimation(anim, 0);
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
