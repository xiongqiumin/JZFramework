#include "JZDialog.h"
#include "JZPainter.h"

JZ_OBJECT_IMPL(JZDialog, JZWindow)

JZDialog::JZDialog(JZWidget* parent)
    : JZWindow(parent)
    , sigAccepted(this)
    , sigRejected(this)
    , sigFinished(this)
    , m_result(Rejected)
{
    setWindowType(Modal);   // 对话框框形 + execModal 禁用 owner
    resize(320, 240);
}

JZDialog::Result JZDialog::result() const
{
    return m_result;
}

int JZDialog::exec()
{
    m_result = Rejected;
    show();
    // 模态循环已收敛到 JZWindow::execModal (泵 Win32 消息 + 框架事件 + 渲染,
    // 禁用/恢复 owner); accept/reject/closeEvent -> hide() 即退出
    execModal();
    return m_result;
}

void JZDialog::accept()
{
    m_result = Accepted;
    sigAccepted.emit();
    sigFinished.emit((int)Accepted);
    hide();
}

void JZDialog::reject()
{
    m_result = Rejected;
    sigRejected.emit();
    sigFinished.emit((int)Rejected);
    hide();
}

// X / Alt+F4: 转为 reject 结束模态; ignore() 让框架不销毁原生窗口
// (只隐藏, 下次 exec 复用)。
void JZDialog::closeEvent(JZCloseEvent* event)
{
    event->ignore();
    reject();
}


void JZDialog::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width(), h = height();
    p.setBrush(JZBrush(JZColor(0xff, 0xff, 0xff)));
    p.setPen(JZPen(JZColor(0x40, 0x40, 0x40), 1));
    p.drawRoundRect(0, 0, w, h, 6);
}
