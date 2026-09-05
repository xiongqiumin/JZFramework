#include "JZPropertyAnimation.h"
#include "JZWidget.h"
#include <algorithm>
#include <cctype>

JZ_OBJECT_IMPL(JZPropertyAnimation, JZAbstractAnimation)

JZPropertyAnimation::JZPropertyAnimation(JZObject* parent)
    : JZAbstractAnimation(parent)
{
}

static std::string jzLower(const std::string& s)
{
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return r;
}

bool JZPropertyAnimation::isSupportedName(const std::string& name)
{
    const std::string n = jzLower(name);
    return n == "x" || n == "y" || n == "w" || n == "width"
        || n == "h" || n == "height";
}

bool JZPropertyAnimation::setProperty(const std::string& name, int32_t from, int32_t to)
{
    if (!isSupportedName(name))
        return false;
    if (!targetIsWidget())
        return false;                       // 属性动画只作用于控件目标
    m_propName = jzLower(name);
    setRange(from, to);
    return true;
}

bool JZPropertyAnimation::setProperty(const std::string& name, int32_t to)
{
    if (!isSupportedName(name) || !targetIsWidget())
        return false;
    m_propName = jzLower(name);
    setRange(currentValue(), to);
    return true;
}

int32_t JZPropertyAnimation::currentValue() const
{
    JZWidget* w = targetWidget();
    if (!w)
        return 0;
    if (m_propName == "x")  return w->x();
    if (m_propName == "y")  return w->y();
    if (m_propName == "w" || m_propName == "width")  return w->width();
    if (m_propName == "h" || m_propName == "height") return w->height();
    return 0;
}

bool JZPropertyAnimation::applyToWidget(int32_t v)
{
    JZWidget* w = targetWidget();
    if (!w)
        return false;

    if (m_propName == "x")
    {
        w->move(v, w->y());
        return true;
    }
    if (m_propName == "y")
    {
        w->move(w->x(), v);
        return true;
    }
    if (m_propName == "w" || m_propName == "width")
    {
        w->resize(v, w->height());
        return true;
    }
    if (m_propName == "h" || m_propName == "height")
    {
        w->resize(w->width(), v);
        return true;
    }
    return false;
}

void JZPropertyAnimation::onValueChanged(int32_t value)
{
    if (applyToWidget(value))
        return;                             // 已作为属性写入目标控件
    JZAbstractAnimation::onValueChanged(value);   // 非控件/未知: 回退 execCb
}
