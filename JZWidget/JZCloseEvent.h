#ifndef JZ_CLOSE_EVENT_H_
#define JZ_CLOSE_EVENT_H_

#include "JZEvent.h"

// 关闭事件 (WM_CLOSE 翻译): 重写 closeEvent 的一方以 accept/ignore 表态
// 是否同意关闭, 真正的关闭动作由 JZWindow::requestClose 统一执行。
// 默认接受 (Qt 语义), 否决 (如"是否保存"取消) 在 handler 里 ignore()。
class JZCloseEvent : public JZEvent
{
public:
    JZCloseEvent() : JZEvent(JZEvent_close) {}
};

#endif
