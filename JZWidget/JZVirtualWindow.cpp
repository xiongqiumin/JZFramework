#include "JZVirtualWindow.h"
#include "JZVirtualDevice.h"
#include "JZDisplayDevice.h"

// ==================== 虚拟桌面上的顶层窗口 ====================
// 只复用 JZWindow 的**控件树与分发逻辑** (hitTest / dispatchMouse / dispatchKey /
// 焦点 / 输入蒙版), 不复用其平台窗口行为: showEvent 不调 createWindow(),
// 所以整个生命周期里没有任何 HWND (nativeHandle() 恒为 nullptr)。
//
// 渲染与输入都**不分叉**: 重绘仍走 JZWindow::doPaint, 输入仍走 onWindowProcess,
// 因此虚拟窗口与真实窗口的行为不会漂移 —— 这是"脚本能精确重复"的前提。

JZVirtualWindow::JZVirtualWindow(JZVirtualDevice* desktop, int w, int h,
                                 const std::string& title)
    : JZWindow(w, h, title, nullptr, SkipTopLevelRegistration())
{
    m_desktop = desktop;
}

JZVirtualWindow::~JZVirtualWindow()
{
}

void JZVirtualWindow::setScreenPos(int x, int y)
{
    if (m_screenX == x && m_screenY == y && m_hasScreenPos)
        return;
    m_screenX = x;
    m_screenY = y;
    m_hasScreenPos = true;
    if (m_desktop)
        m_desktop->onWindowFrameChanged(this);   // 位置变了 => 桌面重新合成
}

// 首次 show: 建离屏帧缓冲 (不建原生窗口); 之后重入只做重绘。
void JZVirtualWindow::showEvent(JZShowEvent* event)
{
    // 只走控件层的可见性变化 —— 不调 JZWindow::showEvent, 因此不 createWindow()
    JZWidget::showEvent(event);

    if (width() <= 0 || height() <= 0)
        resize(320, 240);

    if (m_device) {
        // 虚拟态: 逻辑单位 == 像素。固定 scale = 1, 让渲染与桌面 DPI 无关,
        // 取帧/注入的坐标才可复现。
        m_device->setScale(1.0f);
        m_device->initOffscreen(width(), height());
        m_offscreenReady = true;
    }

    // 放行 doPaint。重绘入口与真实窗口完全一致 (不因"虚拟"而分叉)。
    m_windowCreated = true;

    renderNow();
}

void JZVirtualWindow::resize(int w, int h)
{
    const bool changed = (w != width() || h != height());
    JZWidget::resize(w, h);          // 控件层: 派发 resizeEvent -> 重新布局
    if (!changed)
        return;
    if (m_device && m_offscreenReady)
        m_device->resize(w, h);      // 重建离屏帧缓冲 (scale = 1 => 物理 = 逻辑)
    if (m_windowCreated) {
        renderNow();
        if (m_desktop)
            m_desktop->onWindowFrameChanged(this);
    }
}

bool JZVirtualWindow::renderNow()
{
    if (!m_device || !m_windowCreated)
        return false;
    doPaint(rect());                 // 遍历控件树 -> paintEvent -> 后端回放
    return true;
}

JZImage JZVirtualWindow::snapshot(bool renderFirst)
{
    if (renderFirst)
        renderNow();
    JZImage img;
    if (m_device)
        m_device->snapshot(img);     // 直接读本窗口的帧缓冲
    return img;
}

// ---- 输入注入 ----
// 与平台消息层的差别只有一处: 事件在这里构造, 坐标已是**本窗口客户区**逻辑坐标
// (真实路径是从 lParam 的物理像素除以 scale 得来)。之后完全同一条分发链。
bool JZVirtualWindow::injectMouse(JZMouseEvent::MouseType type, JZMouseEvent::Button button,
                                  int x, int y, int wheelDelta, uint32_t modifiers)
{
    JZMouseEvent ev(type, button, JZPoint(x, y), JZPoint(), wheelDelta, modifiers);
    ev.setProgrammatic(true);        // 与 AI 合成点击同义: 不受输入蒙版屏蔽
    const bool handled = onWindowProcess(&ev);
    if (type == JZMouseEvent::Press || type == JZMouseEvent::Release) {
        renderNow();
        if (m_desktop)
            m_desktop->onWindowFrameChanged(this);   // 点击后立即让桌面出帧
    }
    return handled;
}

bool JZVirtualWindow::injectClick(int x, int y, JZMouseEvent::Button button)
{
    const bool down = injectMouse(JZMouseEvent::Press, button, x, y);
    const bool up = injectMouse(JZMouseEvent::Release, button, x, y);
    return down || up;
}

bool JZVirtualWindow::injectKey(uint32_t key, const std::string& text,
                                uint32_t modifiers, bool press)
{
    JZKeyEvent ev(key, modifiers, text);
    ev.setPress(press);
    const bool handled = onWindowProcess(&ev);
    renderNow();
    if (m_desktop)
        m_desktop->onWindowFrameChanged(this);
    return handled;
}

void JZVirtualWindow::injectFocus(bool got)
{
    // 与 WM_SETFOCUS / WM_KILLFOCUS 同一条路径 (只是没有 Win32 参与)
    onWindowFocus(got);
    if (m_desktop)
        m_desktop->onWindowFrameChanged(this);
}
