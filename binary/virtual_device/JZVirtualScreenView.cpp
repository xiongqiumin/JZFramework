// ==================== 虚拟桌面的"显示器"控件 ====================
#include "JZVirtualScreenView.h"
#include "JZVirtualDevice.h"
#include "JZPainter.h"
#include "JZWindow.h"

JZVirtualScreenView::JZVirtualScreenView(JZVirtualDevice* screen, JZWidget* parent)
    : JZWidget(parent), m_screen(screen)
{
    if (m_screen)
        m_screen->setView(this);
}

JZVirtualScreenView::~JZVirtualScreenView()
{
    // 桌面只借用显示控件 (谁建谁持有): 先解绑, 避免留下悬垂的 m_view
    if (m_screen && m_screen->view() == this)
        m_screen->setView(nullptr);
}

void JZVirtualScreenView::setScreen(JZVirtualDevice* screen)
{
    if (m_screen && m_screen->view() == this)
        m_screen->setView(nullptr);
    m_screen = screen;
    if (m_screen)
        m_screen->setView(this);
    update();
}

JZSize JZVirtualScreenView::sizeHint() const
{
    if (m_screen)
        return m_screen->desktopSize();
    return JZSize(800, 600);
}

JZPoint JZVirtualScreenView::toScreen(const JZPoint& local) const
{
    // 1:1 显示: 视图坐标系与桌面坐标系重合 (原点都在左上角)。
    // 若将来要做缩放/居中, 只改这一处 —— 事件与绘制必须走同一个换算。
    return local;
}

void JZVirtualScreenView::paintEvent(JZPainterEvent* event)
{
    if (!m_screen)
        return;
    const JZImage& img = m_screen->desktopImage();
    if (img.isNull() || !img.data())
        return;
    JZPainter p(this);
    p.drawImage(img, 0, 0, img.width(), img.height());
}

// ==================== 鼠标: 换算成桌面坐标 -> 交给窗口管理器 ====================
void JZVirtualScreenView::mousePressEvent(JZMouseEvent* event)
{
    if (!m_screen) {
        JZWidget::mousePressEvent(event);
        return;
    }
    const JZPoint p = toScreen(event->localPos());
    if (m_screen->injectMouseAtScreen(JZMouseEvent::Press, event->button(),
                                      p.x(), p.y(), 0, event->modifiers())) {
        grabMouse();          // 拖动/缩放/控件拖拽: 冲出视图也要收到 move/up
        event->accept();
    }
}

void JZVirtualScreenView::mouseReleaseEvent(JZMouseEvent* event)
{
    if (!m_screen) {
        JZWidget::mouseReleaseEvent(event);
        return;
    }
    const JZPoint p = toScreen(event->localPos());
    m_screen->injectMouseAtScreen(JZMouseEvent::Release, event->button(), p.x(), p.y(),
                                  0, event->modifiers());
    releaseMouse();
    event->accept();
}

void JZVirtualScreenView::mouseMoveEvent(JZMouseEvent* event)
{
    if (!m_screen) {
        JZWidget::mouseMoveEvent(event);
        return;
    }
    const JZPoint p = toScreen(event->localPos());
    m_screen->injectMouseAtScreen(JZMouseEvent::Move, event->button(), p.x(), p.y(),
                                  0, event->modifiers());

    // 光标形状跟着命中区域走 (边框 = 缩放箭头), 与真窗口拖动时的反馈一致
    const JZCursorShape want = m_screen->cursorAt(p.x(), p.y());
    if (cursor().shape() != want) {
        setCursor(want);
        if (JZWindow* w = window())
            w->applyMouseCursor();
    }
    event->accept();
}

void JZVirtualScreenView::wheelEvent(JZMouseEvent* event)
{
    if (!m_screen) {
        JZWidget::wheelEvent(event);
        return;
    }
    const JZPoint p = toScreen(event->localPos());
    const bool handled = m_screen->injectMouseAtScreen(JZMouseEvent::Wheel, event->button(),
                                                       p.x(), p.y(), event->wheelDelta(),
                                                       event->modifiers());
    if (handled)
        event->accept();
    else
        event->ignore();      // 桌面上那个点不归任何窗口 -> 交给外层滚动区滚
}

// ==================== 键盘: 发给桌面上当前的活动窗口 ====================
void JZVirtualScreenView::keyPressEvent(JZKeyEvent* event)
{
    if (!m_screen) {
        JZWidget::keyPressEvent(event);
        return;
    }
    if (m_screen->injectKey(event->key(), event->text(), event->modifiers(), true))
        event->accept();
    else
        event->ignore();
}

void JZVirtualScreenView::keyReleaseEvent(JZKeyEvent* event)
{
    if (!m_screen) {
        JZWidget::keyReleaseEvent(event);
        return;
    }
    if (m_screen->injectKey(event->key(), event->text(), event->modifiers(), false))
        event->accept();
    else
        event->ignore();
}
