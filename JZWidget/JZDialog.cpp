#include "JZDialog.h"
#include "JZPainter.h"
#include "JZApplication.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

JZ_OBJECT_IMPL(JZDialog, JZWindow)

JZDialog::JZDialog(JZWidget* parent)
    : JZWindow(parent)
    , sigAccepted(this)
    , sigRejected(this)
    , sigFinished(this)
    , m_result(Rejected)
{
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

    // Win32 原生模态 (DialogBoxParam 语义): 对话框创建时已挂 owner
    // (JZWindow::createWindow 把父链所属窗口传给 hWndParent), 这里禁用 owner
    // —— 系统不把输入交给禁用窗口, 消息泵无需丢任何消息; 退出时恢复并交回激活。
    HWND owner = m_hwnd ? GetWindow((HWND)m_hwnd, GW_OWNER) : nullptr;
    bool ownerDisabled = (owner && IsWindowEnabled(owner));
    if (ownerDisabled)
        EnableWindow(owner, FALSE);

    // 模态事件循环 (参考 JZWin32Thread::run): 嵌套循环必须自己泵 Win32 消息。
    // 不能用 JZEventLoop::exec —— 其 waitEvents 只在条件变量上等待, 期间对话框
    // HWND 的输入/关闭消息无人分发 (窗口假死), 也没有 collectInvaildEvents /
    // dealPaint 渲染管线, 画不出来。
    bool finished = false;
    connect(&sigFinished, [&](int) { finished = true; });

    MSG msg;
    while (!finished && m_hwnd)
    {
        // 1. 泵干本线程全部 Win32 消息 (对话框输入 / WM_CLOSE / IME)
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)   // 应用退出: 归还消息并结束模态
            {
                PostQuitMessage((int)msg.wParam);
                finished = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (finished)
            break;

        // 2. 框架事件队列 + 到期定时器 (光标闪烁等)
        jzApp->processEvents();

        // 3. 渲染: 嵌套循环拿不到主循环的合并脏区管线, 直接整窗重绘
        //    (对话框窗口小, 空闲 30ms 一帧成本可忽略)
        doPaint(rect());

        // 4. 等待新消息或至多 30ms (等效主循环心跳, 避免忙等)
        MsgWaitForMultipleObjectsEx(0, nullptr, 30, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    hide();

    // 恢复 owner 并交回激活
    if (ownerDisabled) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }

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
