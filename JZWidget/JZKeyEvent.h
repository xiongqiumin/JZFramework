#ifndef JZ_KEY_EVENT_H_
#define JZ_KEY_EVENT_H_

#include <inttypes.h>
#include "JZEvent.h"

class JZKeyEvent : public JZEvent
{
public:
    explicit JZKeyEvent();

    uint32_t key() const;
    bool isAccepted() const;
    void accept();
    void ignore();

private:
    bool m_accepted = false;
};

#endif // JZ_KEY_EVENT_H_
