#ifndef JZ_EVENT_H_
#define JZ_EVENT_H_

enum {
    JZEvent_none,
    JZEvent_deleteObject,
    JZEvent_timer,
    JZEvent_asyncSignal,
    JZEvent_netReq,
    JZEvent_netRes,
    JZEvent_log,
    JZEvent_co,
    JZEvent_paint,
    JZEvent_show,
    JZEvent_hide,
    JZEvent_resize,
    JZEvent_key,
    JZEvent_user = 1000,
};

#define INVALID_TIMER_ID (0)

class JZEvent
{
public:
    JZEvent(int type);
    virtual ~JZEvent();

    int type() const;

protected:
    int m_type;
};

class JZTimerEvent : public JZEvent
{
public:
    JZTimerEvent(int id);
    ~JZTimerEvent();

    int id() const;

protected:
    int m_id;
};

#endif // !JZ_EVENT_H_