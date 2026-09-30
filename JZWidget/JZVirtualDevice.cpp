#include "JZVirtualDevice.h"
#include "JZImageLabel.h"
#include "JZDisplayDevice.h"

// ==================== 虚拟设备 ====================
// 只复用 JZWindow 的**控件树与分发逻辑** (hitTest / dispatchMouse / dispatchKey /
// 焦点 / 输入蒙版), 不复用它的平台窗口行为: showEvent 不调 createWindow(),
// 所以整个生命周期里没有任何 HWND (nativeHandle() 恒为 nullptr)。
//
// 这样做的意义: 运行期控制与取帧都不再经过像素通道 (寻址靠控件树, 取帧靠帧缓冲),
// 同时因为是同一套分发/绘制代码, 虚拟态与真实窗口下的行为不会漂移。

JZVirtualDevice::JZVirtualDevice(int w, int h, const std::string& title, JZWidget* parent)
    : JZWindow(w, h, title, parent)
{
}

JZVirtualDevice::~JZVirtualDevice()
{
    hideViewer();
}

// 首次 show: 建离屏帧缓冲 (不建原生窗口); 之后重入只做重绘。
void JZVirtualDevice::showEvent(JZShowEvent* event)
{
    // 只走控件层的可见性变化 —— 不调 JZWindow::showEvent, 因此不 createWindow()
    JZWidget::showEvent(event);

    if (width() <= 0 || height() <= 0)
        resize(800, 600);

    if (m_device) {
        // 虚拟态: 逻辑单位 == 像素。固定 scale = 1 让渲染与桌面 DPI 无关,
        // 回放/取帧的像素坐标才可复现 (多屏 HiDPI 下尤其重要)。
        m_device->setScale(1.0f);
        m_device->initOffscreen(width(), height());
        m_offscreenReady = true;
    }

    // 放行 doPaint。渲染入口与真实窗口完全一致 (不因"虚拟"而分叉)。
    m_windowCreated = true;

    renderNow();
}

void JZVirtualDevice::resize(int w, int h)
{
    const bool changed = (w != width() || h != height());
    JZWidget::resize(w, h);          // 控件层: 派发 resizeEvent -> 重新布局
    if (!changed)
        return;
    if (m_device && m_offscreenReady)
        m_device->resize(w, h);      // 重建离屏帧缓冲 (scale = 1 => 物理 = 逻辑)
    if (m_windowCreated)
        renderNow();
}

bool JZVirtualDevice::renderNow()
{
    if (!m_device || !m_windowCreated)
        return false;
    doPaint(rect());                  // 遍历控件树 -> paintEvent -> 后端回放
    afterFrame();
    return true;
}

JZImage JZVirtualDevice::snapshot(bool renderFirst)
{
    if (renderFirst)
        renderNow();
    JZImage img;
    if (m_device)
        m_device->snapshot(img);      // 直接读帧缓冲
    return img;
}

// ---- 程序化输入注入 ----
// 与平台消息层的差别只有一处: 事件在这里构造, 坐标已是逻辑客户区坐标
// (真实路径是从 lParam 的物理像素除以 scale 得来)。之后完全同一条分发链:
// onWindowProcess -> dispatchMouse -> hitTest -> 控件事件。
bool JZVirtualDevice::injectMouse(JZMouseEvent::MouseType type, JZMouseEvent::Button button,
                                  int x, int y, int wheelDelta, uint32_t modifiers)
{
    JZMouseEvent ev(type, button, JZPoint(x, y), JZPoint(), wheelDelta, modifiers);
    ev.setProgrammatic(true);         // 与 AI 合成点击同义: 不受输入蒙版屏蔽
    const bool handled = onWindowProcess(&ev);
    if (type == JZMouseEvent::Press || type == JZMouseEvent::Release)
        renderNow();                  // 点击后立即出帧, 便于紧接着 snapshot()
    return handled;
}

bool JZVirtualDevice::injectClick(int x, int y, JZMouseEvent::Button button)
{
    const bool down = injectMouse(JZMouseEvent::Press, button, x, y);
    const bool up = injectMouse(JZMouseEvent::Release, button, x, y);
    return down || up;
}

bool JZVirtualDevice::injectKey(uint32_t key, const std::string& text,
                                uint32_t modifiers, bool press)
{
    JZKeyEvent ev(key, modifiers, text);
    ev.setPress(press);
    return onWindowProcess(&ev);
}

void JZVirtualDevice::injectFocus(bool got)
{
    // 与 WM_SETFOCUS / WM_KILLFOCUS 同一条路径 (只是没有 Win32 参与)
    onWindowFocus(got);
}

// ---- 查看器 ----
// 一个普通顶层窗口, 每帧把虚拟设备的帧缓冲贴出来给人看。
// 它不向虚拟设备投递任何输入, 也不参与其时钟/焦点, 因此"能看"与"能确定性回放"
// 不冲突; 不需要时 hideViewer() 关掉即可。
void JZVirtualDevice::showViewer(const std::string& title)
{
    if (!m_viewer) {
        m_viewer = new JZWindow(width() > 0 ? width() : 800,
                                height() > 0 ? height() : 600, title);
        m_viewerLabel = new JZImageLabel(m_viewer);
        m_viewerLabel->setGeometry(0, 0, m_viewer->width(), m_viewer->height());
        m_viewerLabel->show();
        m_viewer->show();
    }
    updateViewer();
}

void JZVirtualDevice::hideViewer()
{
    if (m_viewer) {
        m_viewer->close();
        delete m_viewer;              // 查看器无父对象, 所有权在本设备
        m_viewer = nullptr;
        m_viewerLabel = nullptr;
    }
}

void JZVirtualDevice::updateViewer()
{
    if (!m_viewer || !m_viewerLabel || !m_device)
        return;
    JZImage frame;
    if (m_device->snapshot(frame)) {  // 只读帧缓冲: 不再渲染, 不会重入
        m_viewerLabel->setImage(frame);
        m_viewerLabel->update();
    }
}

void JZVirtualDevice::afterFrame()
{
    updateViewer();
}
