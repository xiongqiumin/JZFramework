#ifndef JZ_KEY_EVENT_H_
#define JZ_KEY_EVENT_H_

#include "lvgl.h"

class JZKeyEvent
{
public:
    explicit JZKeyEvent(lv_event_t* e) : m_event(e) {}

    uint32_t key() const     { return lv_indev_get_key(lv_indev_active()); }
    bool isAccepted() const  { return m_accepted; }
    void accept()            { m_accepted = true; }
    void ignore()            { m_accepted = false; }

private:
    lv_event_t* m_event;
    bool m_accepted = false;
};

#endif // JZ_KEY_EVENT_H_
