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
    // 对话框底色: 整个矩形客户区直接铺白 (系统标题栏/边框由 DWM 绘制)。
    // 不要画圆角矩形/描边 —— 圆角几何覆盖不到四角与边缘取整行, 会与
    // 四周系统边框之间露出白边/虚边 (曾表现为"窗口边上透明边框")。
    JZPainter p(this);
    p.fillRect(rect(), JZColor::White);
}
