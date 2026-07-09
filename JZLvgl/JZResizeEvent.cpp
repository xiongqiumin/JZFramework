#include "JZResizeEvent.h"

JZResizeEvent::JZResizeEvent(const JZSize& size, const JZSize& oldSize)
    : m_size(size), m_oldSize(oldSize)
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
