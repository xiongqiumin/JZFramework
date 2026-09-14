#include "JZResizeEvent.h"

JZResizeEvent::JZResizeEvent(const JZSize& size, const JZSize& oldSize)
    : JZEvent(JZEvent_resize)
    , m_size(size)
    , m_oldSize(oldSize)
{
}

JZSize JZResizeEvent::size() const
{
    return m_size;
}

JZSize JZResizeEvent::oldSize() const
{
    return m_oldSize;
}
