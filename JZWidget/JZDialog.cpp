#include "JZDialog.h"
#include "JZEventLoop.h"
#include "JZPainter.h"
#include "JZApplication.h"

JZ_OBJECT_IMPL(JZDialog, JZWindow)

JZDialog::JZDialog(int w, int h, const std::string& title, JZWidget* parent)
    : JZWindow(w, h, title.empty() ? "Dialog" : title)
    , sigAccepted(this)
    , sigRejected(this)
    , sigFinished(this)
    , m_result(Rejected)
{
    // 构造后先隐藏原生窗口, exec()/show 时再显示
    setWindowVisible(false);

    // parent 仅作内存所有权, 不进入其可视控件树 (遍历/命中检测会跳过设备节点)
    if (parent)
        setParent(parent);
}

JZDialog::Result JZDialog::result() const
{
    return m_result;
}

int JZDialog::exec()
{
    m_result = Rejected;
    setWindowVisible(true);

    // 模态: 屏蔽其它设备窗口输入
    JZDisplayDevice::setOthersInputEnabled(this, false);

    JZEventLoop loop;
    connect(&sigFinished, [&loop](int) { loop.quit(); });
    loop.exec();

    JZDisplayDevice::setOthersInputEnabled(this, true);
    setWindowVisible(false);

    return m_result;
}

void JZDialog::accept()
{
    m_result = Accepted;
    sigAccepted.emit();
    sigFinished.emit((int)Accepted);
    setWindowVisible(false);
}

void JZDialog::reject()
{
    m_result = Rejected;
    sigRejected.emit();
    sigFinished.emit((int)Rejected);
    setWindowVisible(false);
}

void JZDialog::closeEvent(JZCloseEvent*)
{
    reject();
}

void JZDialog::requestClose()
{
    // 原生窗口 WM_CLOSE: 对话框关闭为 reject, 不退出应用
    JZCloseEvent ev;
    closeEvent(&ev);
}

void JZDialog::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width(), h = height();
    p.setBrush(JZBrush(JZColor(0xff, 0xff, 0xff)));
    p.setPen(JZPen(JZColor(0x40, 0x40, 0x40), 1));
    p.drawRoundRect(0, 0, w, h, 6);
}
