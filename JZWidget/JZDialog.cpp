#include "JZDialog.h"
#include "JZEventLoop.h"
#include "JZPainter.h"
#include "JZApplication.h"

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

    JZEventLoop loop;
    connect(&sigFinished, [&loop](int) { loop.quit(); });
    loop.exec();

    hide();

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

void JZDialog::closeEvent(JZCloseEvent*)
{
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
