// ==================== 虚拟桌面窗口后端 ====================
// 这里的每个动作都与 JZWin32WindowBackend 一一对应, 只是落到"虚拟桌面"而不是 Win32:
//   建窗口 -> 建离屏帧缓冲(而不是 CreateWindowExW)
//   show   -> 出首帧并让桌面重新合成(而不是 ShowWindow)
//   移动   -> 改桌面坐标(而不是 SetWindowPos)
//   激活   -> 桌面管理器 raise + 焦点(而不是 SetForegroundWindow)
//   取帧   -> 读自己的帧缓冲(而不是抓屏)
//   输入法/光标/抓取 -> 没有 (空实现)
#include "JZVirtualWindowBackend.h"
#include "JZVirtualWindow.h"
#include "JZVirtualDevice.h"
#include "JZWindow.h"
#include "JZDisplayDevice.h"
#include "JZThread.h"    // runOnObject: 把脏区投到窗口自己的线程

JZVirtualWindowBackend::JZVirtualWindowBackend(JZVirtualWindow* vwin)
    : m_vwin(vwin)
{
}

JZVirtualWindowBackend::~JZVirtualWindowBackend()
{
}

// 虚拟侧的句柄就是那个窗口对象 —— "虚拟窗口没有句柄"是过去的事。
void* JZVirtualWindowBackend::handle() const
{
    return m_vwin;
}

bool JZVirtualWindowBackend::create(JZWindow* w)
{
    if (!w || !w->device())
        return false;

    // 虚拟态: 逻辑单位 == 像素。固定 scale = 1, 让渲染与宿主显示器 DPI 无关 ——
    // 取帧与注入的坐标才可复现 (这是"脚本能精确重复"的前提)。
    w->device()->setScale(1.0f);
    const int cw = w->width() > 0 ? w->width() : 320;
    const int ch = w->height() > 0 ? w->height() : 240;
    w->device()->initOffscreen(cw, ch);

    m_created = true;
    return true;
}

void JZVirtualWindowBackend::destroy(JZWindow* w)
{
    (void)w;
    m_created = false;      // 没有原生资源要释放; 之后 show 会重建帧缓冲
}

void JZVirtualWindowBackend::onPlatformDestroyed(JZWindow* w)
{
    (void)w;
    m_created = false;
}

// 显示 (等价 ShowWindow): 桌面上的"让窗口出现"就这一个动作 ——
//   ① 首次显示时按最终尺寸摆位 (JS 里 new JZWindow 时尺寸还没定, 只能等这会儿);
//   ② 出首帧;
//   ③ 置顶 + 拿键盘焦点 (等价"刚打开的应用窗口"), 让桌面重新合成。
// ⚠ JS 里 `win.show()` 直接走的就是这里 (客户区窗口的 showEvent -> 后端 show) —— 不存在
//   "JS 的 show 少做了置顶/聚焦"的分叉。
void JZVirtualWindowBackend::show(JZWindow* w)
{
    if (!m_created || !w || !m_vwin)
        return;
    m_vwin->applyAutoPosition();
    w->doPaint(w->rect());
    if (JZVirtualDevice* d = m_vwin->screen())
        d->activate(m_vwin);
    else
        m_vwin->notifyScreen();
}

// 脏区: 排给**窗口自己的线程**去画 (见 JZWindowBackend::onInvalidate)。
//   · 跨线程的窗口 (虚拟进程建的) —— 发到那个进程的**事件循环**: 它每轮处理事件队列时
//     就把这一帧画掉。虚拟进程本身就是一个 JZThread (有自己的 exec 循环), 所以这里
//     走的是"正常消息循环"那条路, 而不是在事件派发栈里就地画;
//   · 与宿主同线程的窗口 (宿主自己建的) —— 交给框架默认: 排进主线程的 paint task。
// 两种都满足同一条硬约束: **画发生在帧缓冲所属的那个线程上** (见 onPainted)。
void JZVirtualWindowBackend::onInvalidate(JZWindow* w, const JZRect& rc)
{
    if (!w)
        return;
    JZThread* owner = m_vwin ? m_vwin->process() : nullptr;
    if (!owner)
    {
        JZWindowBackend::onInvalidate(w, rc);      // 同线程: 主线程的 paint task
        return;
    }
    if (m_paintPending.exchange(true))
        return;                                    // 已经有一帧排在它线程上了: 合并
    // ⚠ 用 runOnObject 而不是 runOnThread: 窗口(客户区)在 lambda 跑之前被销毁的话,
    //   这次投递直接作废, 不会拿着悬垂指针去画 (见 JZThread.h 里 runOnObject 的约定)。
    runOnObject(w, [this, w]() {
        m_paintPending = false;
        w->doPaint(w->rect());
    });
}

// 画完一帧: 把新帧拷进窗口的"帧缓存"并通知桌面重合成。
// ⚠ 本函数一定在**窗口自己的线程**上被调 (见 onInvalidate / onPainted 的调用点) ——
//   所以这里的拷贝不跨线程, 不需要等谁。
void JZVirtualWindowBackend::onPainted(JZWindow* w)
{
    (void)w;
    if (!m_vwin)
        return;
    m_vwin->captureFrame();
    m_vwin->notifyScreen();
}

// 隐藏: 没有平台动作 —— 合成时按 isVisible() 跳过它 (桌面底色自然露出来)
void JZVirtualWindowBackend::hide(JZWindow* w)
{
    (void)w;
    if (m_vwin)
        m_vwin->notifyScreen();
}

// 标题: 标题栏由 JZVirtualWindow 画 (本类无事可做); 通知桌面重新合成即可。
void JZVirtualWindowBackend::setTitle(JZWindow* w, const std::string& title)
{
    (void)w;
    (void)title;
    if (m_vwin)
        m_vwin->notifyScreen();
}

void JZVirtualWindowBackend::setClientSize(JZWindow* w, int cw, int ch)
{
    if (!w || !m_created)
        return;
    if (JZDisplayDevice* d = w->device())
        d->resize(cw, ch);      // scale 恒 1 => 物理尺寸 == 逻辑尺寸
    w->doPaint(w->rect());
    if (m_vwin)
        m_vwin->notifyScreen();
}

void JZVirtualWindowBackend::setScreenPos(JZWindow* w, int x, int y)
{
    // 桌面坐标已经由 JZWindow::moveScreen 记在窗口上 (坐标系归桌面, 见 JZVirtualDevice),
    // 这里只需要让桌面按新位置重新合成。
    (void)x;
    (void)y;
    if (m_vwin)
        m_vwin->notifyScreen();
}

JZPoint JZVirtualWindowBackend::clientToScreen(JZWindow* w, const JZPoint& pt) const
{
    (void)w;
    // 客户区 -> 桌面: 口径只有一处 —— JZVirtualWindow::clientToScreen
    return m_vwin ? m_vwin->clientToScreen(pt) : pt;
}

JZPoint JZVirtualWindowBackend::screenToClient(JZWindow* w, const JZPoint& pt) const
{
    (void)w;
    return m_vwin ? m_vwin->screenToClient(pt) : pt;
}

// 虚拟桌面是**纯逻辑桌面**: 逻辑单位 == 像素, 缩放恒 1 (与宿主显示器 DPI 无关)
float JZVirtualWindowBackend::deviceScale(JZWindow* w) const
{
    (void)w;
    return 1.0f;
}

// "该窗口的屏幕矩形是否包含这个点": 与真窗口同口径用**外框** (弹层外部按下关闭等用它)
bool JZVirtualWindowBackend::containsScreenPoint(JZWindow* w, int x, int y) const
{
    (void)w;
    if (!m_vwin)
        return false;
    const JZRect f = m_vwin->frameGeometry();
    return x >= f.left() && x <= f.right() && y >= f.top() && y <= f.bottom();
}

// 置顶: 虚拟桌面只有 z 序, 没有"取消置顶" (on=false 时无事可做)
void JZVirtualWindowBackend::setTop(JZWindow* w, bool on)
{
    (void)w;
    if (on && m_vwin && m_vwin->screen())
        m_vwin->screen()->raise(m_vwin);
}

void JZVirtualWindowBackend::showMinimized(JZWindow* w)
{
    (void)w;
    if (m_vwin)
        m_vwin->setMinimized(true);
}

void JZVirtualWindowBackend::showMaximized(JZWindow* w)
{
    (void)w;
    if (m_vwin)
        m_vwin->setMaximized(true);
}

void JZVirtualWindowBackend::showNormal(JZWindow* w)
{
    (void)w;
    if (!m_vwin)
        return;
    m_vwin->setMaximized(false);
    m_vwin->setMinimized(false);
}

// 激活: 置顶 + 键盘焦点 —— 焦点消息经 setWindowFocus 回流后 isActiveWindow() 即为真,
// 与真实窗口的 activateWindow() 完全同语义 (只是不碰真实系统的前台窗口)。
void JZVirtualWindowBackend::activate(JZWindow* w)
{
    (void)w;
    if (m_vwin)
        m_vwin->activate();
}

void JZVirtualWindowBackend::updateCursor(JZWindow* w) { (void)w; }
void JZVirtualWindowBackend::setCapture(JZWindow* w, bool on) { (void)w; (void)on; }

// 虚拟桌面下没有平台模态循环: 窗口的"模态"由调用方自己控制流程
int JZVirtualWindowBackend::execModal(JZWindow* w, bool disableOwner)
{
    (void)w;
    (void)disableOwner;
    return 0;
}

// 客户区一帧: 直接读本窗口的帧缓冲 (不是抓屏, 也不受遮挡/z 序影响)
JZImage JZVirtualWindowBackend::grabClient(JZWindow* w)
{
    JZImage img;
    if (w && w->device())
        w->device()->snapshot(img);
    return img;
}
