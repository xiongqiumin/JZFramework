// ==================== 虚拟桌面的"显示器"控件 ====================
#include "JZVirtualScreenView.h"
#include "JZVirtualDevice.h"
#include "JZPainter.h"
#include "JZPixmap.h"       // 合成图自管转换 (见 paintEvent)
#include "JZWindow.h"
#include "JZDisplayDevice.h"

#include <cmath>

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
    // 1:1 显示: 视图坐标系与桌面坐标系重合 (原点都在左上角)。DPI 不进这里 ——
    // 缩放同步给了桌面 (见 syncHostScale), 桌面坐标始终是逻辑的, 于是换算只有一个恒等映射。
    return local;
}

// 与主机共享 DPI: 宿主窗口当前的设备缩放 -> 桌面 (桌面据此渲染物理帧缓冲)。
// ⚠ 宿主 DPI 变了才动 (setScale 内部还有一道等值判断), 平时就是两次浮点比较。
void JZVirtualScreenView::syncHostScale()
{
    if (!m_screen)
        return;
    JZWindow* w = window();
    if (!w || !w->device())
        return;
    const float s = w->device()->scale();
    if (std::fabs(s - m_screen->scale()) > 0.001f)
        m_screen->setScale(s);
}

// 立即重绘 (同步出帧): 拖动/缩放窗口时用 —— 见 JZVirtualDevice::refreshNow。
void JZVirtualScreenView::paintNow()
{
    if (!isVisible())
        return;
    if (JZWindow* w = window())
        w->doPaint(w->rect());   // 同步走一遍本窗口的绘制 (含本控件的 paintEvent)
}

void JZVirtualScreenView::paintEvent(JZPainterEvent* event)
{
    if (!m_screen)
        return;
    syncHostScale();                     // 宿主的 DPI 可能刚变 (换显示器/改缩放)

    // 合成图是画布设备帧缓冲的**零拷贝位图** (见 JZVirtualDevice::desktopPixmap):
    // 画布在自己的原生缓冲上画完, 这里直接贴 —— 没有 RGBA 转换, 也没有每帧分配。
    const JZPixmap& pm = m_screen->desktopPixmap();
    if (pm.isNull())
        return;

    JZPainter p(this);
    // 目标矩形用**逻辑**尺寸 (控件尺寸), 位图是**物理**像素 (desktopSize * 缩放):
    // 宿主的世界变换 (Scale(hostScale)) 把它 1:1 映射到屏幕像素 —— 不重采样, 文字清晰。
    p.drawPixmap(pm, 0, 0, m_screen->desktopWidth(), m_screen->desktopHeight());
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
