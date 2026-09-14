#ifndef JZ_RESIZE_EVENT_H_
#define JZ_RESIZE_EVENT_H_

#include "JZEvent.h"
#include "JZSize.h"

// 尺寸变化事件 (JZWidget::resize 派发; 见 JZWidget::event 的分派表)。
// 与 JZShowEvent / JZHideEvent 一样是 JZEvent 派生: 事件进入对象的统一入口是
// JZApplication::sendEvent -> JZThread::notify, 那里先过事件过滤器链再进 event()/xxxEvent(),
// 所以 resize 也必须能当 JZEvent 派发 —— 否则过滤器(JS 侧 onResize)看不到这个事件。
class JZResizeEvent : public JZEvent
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
