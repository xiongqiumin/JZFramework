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
    JZEvent_invalidate,
    JZEvent_paint,
    JZEvent_show,
    JZEvent_hide,
    JZEvent_resize,
    JZEvent_key,
    JZEvent_mouse,
    JZEvent_close,
    JZEvent_user = 1000,
};

#define INVALID_TIMER_ID (0)

class JZEvent
{
public:
    JZEvent(int type);
    virtual ~JZEvent();

    int type() const;

    // 事件表态 (Qt 语义): 默认接受, handler 里 ignore() 才否决。
    // "无人处理需逐级上传"的事件类型 (键盘/IME) 在各自构造里改为默认 ignore(),
    // 由处理成功的 handler accept() 表示已消费。
    bool isAccepted() const { return m_accepted; }
    void accept() { m_accepted = true; }
    void ignore() { m_accepted = false; }

protected:
    int m_type;
    bool m_accepted = true;
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