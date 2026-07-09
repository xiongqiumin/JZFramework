#ifndef JZ_KEY_EVENT_H_
#define JZ_KEY_EVENT_H_

#include <cstdint>

// Forward-declare LVGL event type
typedef struct _lv_event_t lv_event_t;

class JZKeyEvent
{
public:
    explicit JZKeyEvent(lv_event_t* e);

    uint32_t key() const;
    bool isAccepted() const;
    void accept();
    void ignore();

private:
    lv_event_t* m_event;
    bool m_accepted = false;
};

#endif // JZ_KEY_EVENT_H_
