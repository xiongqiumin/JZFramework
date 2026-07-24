#ifndef JZ_RESIZE_EVENT_H_
#define JZ_RESIZE_EVENT_H_

#include "JZSize.h"

class JZResizeEvent
{
public:
    JZResizeEvent(const JZSize& size, const JZSize& oldSize);

    JZSize size() const;
    JZSize oldSize() const;

private:
    JZSize m_size;
    JZSize m_oldSize;
};

#endif
