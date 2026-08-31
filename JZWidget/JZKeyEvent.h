#ifndef JZ_KEY_EVENT_H_
#define JZ_KEY_EVENT_H_

#include <inttypes.h>
#include "JZEvent.h"

class JZKeyEvent : public JZEvent
{
public:
    explicit JZKeyEvent(uint32_t key = 0);

    uint32_t key() const;
    bool isAccepted() const;
    void accept();
    void ignore();

private:
    uint32_t m_key = 0;
    bool m_accepted = false;
};

#endif // JZ_KEY_EVENT_H_
