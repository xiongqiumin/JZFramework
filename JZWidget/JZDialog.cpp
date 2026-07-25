#include "JZDialog.h"
#include "JZEventLoop.h"
#include "JZPainter.h"

JZ_OBJECT_IMPL(JZDialog, JZWidget)

JZDialog::JZDialog(JZWidget* parent)
    : JZWidget(parent)
    , sigAccepted(this)
    , sigRejected(this)
    , sigFinished(this)
    , m_result(Rejected)
{
}

JZDialog::Result JZDialog::result() const
{
    return m_result;
}

int JZDialog::exec()
{
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
}

void JZDialog::reject()
{
    m_result = Rejected;
    sigRejected.emit();
    sigFinished.emit((int)Rejected);
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
