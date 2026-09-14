#include "JZDialog.h"
#include "JZPainter.h"
#include "JZDebug.h"
#include "JZLog.h"

JZ_OBJECT_IMPL(JZDialog, JZWindow)

JZDialog::JZDialog(JZWidget* parent)
    : JZWindow(parent)
    , sigAccepted(this)
    , sigRejected(this)
    , sigFinished(this)
    , m_result(Rejected)
{
    setWindowType(Modal);   // 对话框框形 + execModal 禁用 owner
    // 不预置尺寸: 留 0×0, 首次 showEvent 按 sizeHint 自适应 (见 JZWindow::showEvent)。
    // 使用布局的对话框会自动贴合内容; 绝对定位的对话框 (如 JZMessageBox) 需自行 resize。
}

JZDialog::Result JZDialog::result() const
{
    return m_result;
}

int JZDialog::exec()
{
    // 对齐 Qt (QDialog::exec 源码): 模态循环进行中再对**同一对象**调 exec() —— 打警告 + 返回 -1,
    // 并且不碰 m_result / show() / execModal(), 所以外层那次完全不受影响。
    //   Qt: if (d->eventLoop) { qWarning("QDialog::exec: Recursive call detected"); return -1; }
    // 会走到这里通常只有两种: 模态期间定时器/回调里又调了同一个 dlg.exec(); 或代码写错了。
    // (不同对话框互相嵌套开模态是允许的: 各自的 execModal 独立栈帧。)
    if (m_inExec)
    {
        jzWarning() << "JZDialog::exec: Recursive call detected";
        return -1;
    }

    struct ExecFlag
    {
        bool& flag;
        ExecFlag(bool& f) : flag(f) { flag = true; }
        ~ExecFlag() { flag = false; }
    } in_exec(m_inExec);

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

// ESC 关闭对话框 (与 Qt QDialog 一致: ESC -> reject)。
void JZDialog::keyPressEvent(JZKeyEvent* event)
{
    if (event->key() == JZKey::Key_Escape)
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
