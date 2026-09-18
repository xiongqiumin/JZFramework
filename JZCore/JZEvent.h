#ifndef JZ_EVENT_H_
#define JZ_EVENT_H_

class JZObject;   // 前向声明: JZChildEvent 仅持有 JZObject* 指针

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
    JZEvent_enter,      // 鼠标进入控件 (Qt 的 QEvent::Enter)
    JZEvent_leave,      // 鼠标离开控件 (Qt 的 QEvent::Leave)
    JZEvent_close,
    JZEvent_childAdded,     // 子对象加入 (Qt QEvent::ChildAdded)
    JZEvent_childRemoved,   // 子对象移除 (Qt QEvent::ChildRemoved)
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

// 鼠标进入/离开控件 (同 Qt 的 QEvent::Enter / QEvent::Leave)。
// 由 JZWindow 在命中控件变化 (hitTest) 时派发: 离开旧控件链、进入新控件链
// (含各自祖先, 止于两者最近公共祖先)。无坐标, 仅作"悬停态"通知。
class JZEnterEvent : public JZEvent
{
public:
    JZEnterEvent() : JZEvent(JZEvent_enter) {}
};

class JZLeaveEvent : public JZEvent
{
public:
    JZLeaveEvent() : JZEvent(JZEvent_leave) {}
};

// 子对象增删 (同 Qt 的 QEvent::ChildAdded / QEvent::ChildRemoved)。
// 由 JZObject::addChild / removeChild 同步派发到父对象 (this),
// 父对象在 childEvent(JZChildEvent*) 里收 (如 JZSplitter 借此摘除事件过滤器)。
class JZChildEvent : public JZEvent
{
public:
    JZChildEvent(int type, JZObject *child);
    ~JZChildEvent();

    JZObject *child() const;

protected:
    JZObject *m_child;
};

#endif // !JZ_EVENT_H_