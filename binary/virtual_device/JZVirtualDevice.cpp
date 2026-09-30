// ==================== 虚拟桌面 (虚拟显示器 + 窗口管理器) ====================
// 分层: 桌面底色 + 窗口合成图 (m_desktop, 一张 JZImage)
//          └─ JZVirtualWindow (外框: 标题栏 + 边框 + 三个按钮)
//               └─ 真 JZWindow (客户区: 控件树/渲染/输入)
//
// 坐标: 桌面坐标 == 屏幕逻辑坐标 (左上角原点, scale 恒为 1)。窗口有自己的桌面坐标,
// 所以 screenToClient / clientToScreen / 命中测试 / 归一化 rect 全都有意义 ——
// 与真实桌面同构, CU 的"屏幕坐标 + 窗口"语义可以直接落在这块虚拟桌面上。
#include "JZVirtualDevice.h"
#include "JZVirtualWindow.h"
#include "JZVirtualWindowBackend.h"   // 收窗口时要把客户区的后端换成它
#include "JZVirtualScreenView.h"
#include "JZPainter.h"
#include "JZWindow.h"
#include "JZApplication.h"   // refresh 转投主线程 / 判断当前线程
#include "JZThread.h"
#include "JZLog.h"

#include <algorithm>

namespace {

int clampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

} // namespace

JZVirtualDevice::JZVirtualDevice(int desktopW, int desktopH)
    : m_desktopBg(255, 255, 255, 255)
{
    m_desktopW = desktopW > 0 ? desktopW : 800;
    m_desktopH = desktopH > 0 ? desktopH : 600;
    m_desktop = JZImage::create(m_desktopW, m_desktopH, m_desktopBg);
}

JZVirtualDevice::~JZVirtualDevice()
{
    // ① 先收掉所有进程: 它们的线程里还跑着 JS, 必须先停下来 (否则线程会回头碰这块
    //    正在析构的桌面)。terminateProcess 会等线程结束并收掉它的窗口。
    shutdownProcesses();

    // ② 桌面自己没了: 把这个"窗口宿主"从**本线程**摘掉 —— 否则之后 new 出来的窗口
    //    还会来找一块已经析构的桌面 (悬垂)。
    if (jzWindowHost() == this)
        jzSetWindowHost(nullptr);

    // ③ 桌面持有窗口 (它们没有别的父对象)
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    std::vector<JZVirtualWindow*> list = m_windows;
    m_windows.clear();
    m_active = nullptr;
    m_pressed = nullptr;
    m_dragWin = nullptr;
    for (auto* w : list)
        delete w;
}

// ==================== 进程管理 ====================
// 桌面不认识 JS 引擎: 进程就是"一个 JZThread" —— 停它/等它/删它都是 JZThread 的接口,
// 具体是什么进程 (JZVirtualProcess: 线程 + JZEngineRuntime) 由 jz_virtual_process 决定。
void JZVirtualDevice::addProcess(JZThread* p)
{
    if (!p)
        return;
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    if (std::find(m_processes.begin(), m_processes.end(), p) == m_processes.end())
        m_processes.push_back(p);
}

void JZVirtualDevice::terminateProcess(JZThread* p)
{
    if (!p)
        return;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        if (std::find(m_processes.begin(), m_processes.end(), p) == m_processes.end())
            return;
    }

    // ① 停线程 + 等它真正结束 —— 之后它再也不会碰桌面 (引擎也在那个线程里销毁了)
    p->quit();          // JZThread::quit = 请求退出(协作式) + 唤醒事件循环
    p->wait();          // join

    // ② 收掉它建的窗口 (客户区可能已被 JS 侧回收, destroyWindow/prune 都能处理)
    std::vector<JZVirtualWindow*> mine;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        for (auto* w : m_windows) {
            if (w->process() == p)
                mine.push_back(w);
        }
    }
    for (auto* w : mine)
        destroyWindow(w);

    // ③ 摘表 + 删掉进程对象 (线程已 join; JZThread 的析构是虚的, 删得对)
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        m_processes.erase(std::remove(m_processes.begin(), m_processes.end(), p),
                          m_processes.end());
    }
    delete p;
    refresh();
}

void JZVirtualDevice::shutdownProcesses()
{
    std::vector<JZThread*> list;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        list = m_processes;
    }
    for (auto* p : list)
        terminateProcess(p);      // 会把自己从表里摘掉
}

void JZVirtualDevice::setDesktopBackground(const JZColor& c)
{
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        m_desktopBg = c;
    }
    refresh();
}

void JZVirtualDevice::setView(JZVirtualScreenView* view)
{
    m_view = view;
}

// 合成只允许在**主线程**跑 (帧的读取要稳定, 也不能在别的线程上碰显示控件)。
// 别的线程(进程线程)改了桌面时, 把刷新**转投**回主线程 —— 去抖: 一批改动只合一次。
void JZVirtualDevice::refresh()
{
    JZThread* main = jzApp ? jzApp->mainThread() : nullptr;
    if (main && JZThread::currentThread() != main)
    {
        // ⚠ 这条路径**必须无锁**: 进程线程推帧/改窗口状态时, 主线程可能正卡在
        //   sendEvent 等它 (输入投递) —— 再去抢 m_mutex 就是死锁。
        if (!m_refreshPosted.exchange(true))
            runOnObject(this, [this]() {          // 对象没了就不会跑 (守卫)
                m_refreshPosted = false;
                refreshNow();
            });
        return;
    }
    refreshNow();
}

void JZVirtualDevice::refreshNow()
{
    compose();
    if (m_view)
        m_view->update();     // 视图绘制时读 desktopImage(), 这里只需让它重绘
}

// ==================== 窗口宿主: 新窗口落到这块桌面上 ====================
// ⭐ 这就是"在 JS 引擎里把窗口后端设成 JZVirtualWindowBackend"的那一步:
//    JZWindow 构造的最后一步回调宿主 (见 JZWindowHost), 于是**任何**新建的窗口
//    (JS 里 `new JZWindow(...)`、C++ 里 `new JZWindow(...)`、uiLoader、菜单弹层...)
//    都走这里 —— 收进桌面 = 建外框对象 + 换虚拟后端 + 标成内部窗口 + 登记 z 序。
void JZVirtualDevice::onWindowCreated(JZWindow* w)
{
    adoptWindow(w, nullptr);     // 当前线程就是它的主人 (同线程窗口)
}

// ⭐ 收下一个窗口 (可以在**任何线程**上被调: 进程线程里 new JZWindow 时就发生在那儿)。
// 内部用一把递归锁罩住窗口表, 于是"进程线程建窗"与"主线程合成"不会打架。
JZVirtualWindow* JZVirtualDevice::adoptWindow(JZWindow* client, JZThread* owner)
{
    if (!client)
        return nullptr;

    std::lock_guard<std::recursive_mutex> lk(m_mutex);

    // ① 外框对象: 从此刻起这台窗口有"虚拟侧的句柄"(JZVirtualWindow*), 由桌面管 z 序/装饰
    JZVirtualWindow* vw = new JZVirtualWindow(this, client);
    vw->setProcess(owner);       // 归属: 空 = 同线程窗口
    if (m_hasAdoptPos)
        vw->setFramePos(m_adoptPos.x(), m_adoptPos.y());  // createWindow 显式给的坐标
    else
        vw->setAutoPosition(true);   // 否则等首次 show: 那时尺寸才定, 才能正确居中级联

    // ② 换平台后端: 坐标/显隐/激活/取帧/句柄全部落到这块桌面上 (不再有原生窗口)
    client->setWindowBackend(new JZVirtualWindowBackend(vw));

    // ③ 内部窗口: 不登记进宿主 app 的顶层窗口表, 也不参与其"活动窗口 / 最后一个窗口关闭"判定
    client->setTopLevelWindow(false);

    // ④ 登记 (z 序最上)。此刻还没 show: 合成/命中都会按可见性跳过它。
    m_windows.push_back(vw);
    return vw;
}

// 新窗口的默认位置: 居中 + 按已有窗口数级联 (免得新窗口完全叠住旧窗口)。
// 由 JZVirtualWindow 首次 show 时按**最终**客户区尺寸取用 (构造期尺寸往往还是 0×0)。
JZPoint JZVirtualDevice::defaultWindowPos(int clientW, int clientH) const
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    const int n = (int)m_windows.size() - 1;      // 本窗口已经在表里了
    const int off = (n > 0 ? n : 0) * 24;
    const int fw = (clientW > 0 ? clientW : 320) + 2 * JZVirtualWindow::kBorder;
    const int fh = (clientH > 0 ? clientH : 240) + JZVirtualWindow::kTitleBar +
                   JZVirtualWindow::kBorder;
    return JZPoint((m_desktopW - fw) / 2 + off, (m_desktopH - fh) / 2 + off);
}

// ==================== 多窗口管理 ====================
JZVirtualWindow* JZVirtualDevice::createWindow(int w, int h, const std::string& title)
{
    return createWindowImpl(w, h, title, false, 0, 0);
}

JZVirtualWindow* JZVirtualDevice::createWindow(int w, int h, const std::string& title,
                                               int x, int y)
{
    return createWindowImpl(w, h, title, true, x, y);
}

// 与 JS 里 `new JZWindow(...)` + `show()` 完全同一条路 (都靠宿主钩子收窗):
// 这里多做的只是"给出坐标 / 立刻 show" —— C++ 调用方要的是一个已经出帧的窗口。
// ⚠ 本设备不一定是"当前窗口宿主"(宿主开关是给"之后所有新窗口"用的, 常在 JS 场景才装);
//   所以这里**临时把自己装上**再 new 客户区, 建完立刻还原 —— 于是 C++ 与 JS 两条路
//   都只有一条收窗逻辑 (onWindowCreated), 不会出现"两套建窗流程"。
namespace {
// 现场保存/还原当前宿主 (RAII: new 抛异常也不把进程级开关搅坏)
struct JZHostScope
{
    JZHostScope(JZWindowHost* host) : m_saved(jzWindowHost()) { jzSetWindowHost(host); }
    ~JZHostScope() { jzSetWindowHost(m_saved); }
    JZWindowHost* m_saved;
};
} // namespace

JZVirtualWindow* JZVirtualDevice::createWindowImpl(int w, int h, const std::string& title,
                                                   bool hasPos, int x, int y)
{
    if (w <= 0) w = 320;
    if (h <= 0) h = 240;

    m_adoptPos = JZPoint(x, y);
    m_hasAdoptPos = hasPos;
    const size_t before = m_windows.size();
    JZWindow* client = nullptr;
    {
        JZHostScope hostScope(this);                // 这一句里新建的窗口归本桌面
        client = new JZWindow(w, h, title);
    }
    m_hasAdoptPos = false;

    if (m_windows.size() <= before) {               // 钩子没生效(不该发生): 别漏掉客户区
        delete client;
        return nullptr;
    }
    JZVirtualWindow* vw = m_windows.back();
    vw->show();     // 尺寸此刻才是最终的: 出首帧 + 置顶 + 拿键盘焦点
    return vw;
}

// 客户区被别处回收了 (JS 侧 GC / deleteLater): 这台窗口跟着从桌面消失。
// ⚠ 必须有: 桌面的窗口表里存的是裸指针, 客户区没了就再也不能用它 (否则合成/命中要解引用野指针)。
void JZVirtualDevice::pruneWindows()
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    for (size_t i = 0; i < m_windows.size(); ) {
        JZVirtualWindow* w = m_windows[i];
        if (w->isClientAlive()) {
            ++i;
            continue;
        }
        if (m_active == w)
            m_active = nullptr;
        if (m_dragWin == w)
            endDrag();
        if (m_pressed == w)
            m_pressed = nullptr;
        m_windows.erase(m_windows.begin() + (long)i);
        delete w;                    // 客户区已死: 本类的析构不会再 delete 一次
    }
    if (!m_active && !m_windows.empty())
        m_active = m_windows.back();
}

void JZVirtualDevice::destroyWindow(JZVirtualWindow* w)
{
    if (!w)
        return;
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    auto it = std::find(m_windows.begin(), m_windows.end(), w);
    if (it == m_windows.end())
        return;

    if (m_pressed == w)
        m_pressed = nullptr;
    if (m_dragWin == w)
        endDrag();

    m_windows.erase(it);
    if (m_active == w) {
        // 活动窗口没了: 退到最上层那个 (等价桌面行为)
        m_active = m_windows.empty() ? nullptr : m_windows.back();
        if (m_active)
            m_active->setFocus(true);
    }
    delete w;
    refresh();
}

void JZVirtualDevice::raise(JZVirtualWindow* w)
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    auto it = std::find(m_windows.begin(), m_windows.end(), w);
    if (it == m_windows.end() || it + 1 == m_windows.end())
        return;                     // 不存在, 或已经在最上
    m_windows.erase(it);
    m_windows.push_back(w);
    refresh();
}

void JZVirtualDevice::activate(JZVirtualWindow* w)
{
    if (!w)
        return;
    std::lock_guard<std::recursive_mutex> lk(m_mutex);   // 进程线程也会调 (show 里)
    raise(w);

    if (m_active == w) {
        if (!w->isActive())
            w->setFocus(true);
        return;
    }
    if (m_active)
        m_active->setFocus(false);    // 旧活动窗口失焦 (派发 focusOut, 取消未上屏的编辑)
    m_active = w;
    w->setFocus(true);
    refresh();
}

void JZVirtualDevice::moveWindow(JZVirtualWindow* w, int x, int y)
{
    if (w)
        w->setFramePos(x, y);         // 窗口自己会通知桌面重新合成
}

JZVirtualWindow* JZVirtualDevice::byTitle(const std::string& title) const
{
    if (title.empty())
        return nullptr;
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    for (auto it = m_windows.rbegin(); it != m_windows.rend(); ++it) {
        if ((*it)->title().find(title) != std::string::npos)
            return *it;
    }
    return nullptr;
}

// z 序自上而下: 末尾最上; 命中区域是**外框** (含标题栏/边框, 与真窗口的窗口矩形同口径)
JZVirtualWindow* JZVirtualDevice::windowAt(int screenX, int screenY) const
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    for (auto it = m_windows.rbegin(); it != m_windows.rend(); ++it) {
        JZVirtualWindow* w = *it;
        if (!w->isVisible())
            continue;
        if (w->hitTest(screenX, screenY) != JZVirtualWindow::HitNone)
            return w;
    }
    return nullptr;
}

// ==================== 坐标换算 ====================
JZPoint JZVirtualDevice::screenToClient(JZVirtualWindow* w, int screenX, int screenY) const
{
    if (!w)
        return JZPoint(screenX, screenY);
    return w->screenToClient(JZPoint(screenX, screenY));
}

JZPoint JZVirtualDevice::clientToScreen(JZVirtualWindow* w, int clientX, int clientY) const
{
    if (!w)
        return JZPoint(clientX, clientY);
    return w->clientToScreen(JZPoint(clientX, clientY));
}

// ==================== 合成 / 取帧 ====================
void JZVirtualDevice::compose()
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);   // 与进程线程的建窗/推帧互斥
    if (m_composing)
        return;
    m_composing = true;

    pruneWindows();     // 客户区已被回收的窗口先从表里摘掉 (之后再遍历才安全)

    // 每帧从"桌面底色"起画 (窗口可能被移走/关掉了, 旧像素必须被盖掉)。
    m_desktop = JZImage::create(m_desktopW, m_desktopH, m_desktopBg);

    {
        // 一次合成一个 painter: 外框与客户区帧都按 z 序由下到上叠 (析构时写回整张图)。
        JZPainter p(m_desktop);
        for (auto* w : m_windows) {
            if (!w->isVisible())
                continue;
            // 外框 (标题栏/边框/按钮) —— 窗口管理器那半边
            w->paintDecoration(p, w == m_active);
            if (w->isMinimized())
                continue;                     // 最小化: 只剩标题栏
            // 客户区: 用窗口**推上来的帧缓存** (它自己的线程在内容变化时拷好, 见
            // JZVirtualWindow::captureFrame) —— 合成因此**不会**跨线程去读别人的帧缓冲,
            // 也就永远不会阻塞在某个进程线程上 (多线程下这一点很要紧)。
            const JZImage& f = w->frame();
            if (f.isNull() || !f.data())
                continue;
            const JZRect c = w->clientGeometry();
            p.drawImage(f, c.left(), c.top(), c.width(), c.height());
        }
    }

    m_composing = false;
}

JZImage JZVirtualDevice::screenshot()
{
    refresh();
    return m_desktop;                    // 与 m_desktop 共享像素缓冲, 拷贝很轻
}

JZImage JZVirtualDevice::screenshotWindow(JZVirtualWindow* w)
{
    return w ? w->snapshot(true) : JZImage();
}

void JZVirtualDevice::onWindowFrameChanged(JZVirtualWindow*)
{
    if (m_composing)
        return;                          // 合成过程中的通知忽略 (防重入)
    // 帧内容由窗口自己的线程在画完后端回调里拷好 (见 JZVirtualWindowBackend::onPainted);
    // 这里只请求重新合成 —— 别的线程调用时会自动转投回主线程。
    refresh();
}

// ==================== 输入 ====================
bool JZVirtualDevice::injectMouseAtScreen(JZMouseEvent::MouseType type,
                                          JZMouseEvent::Button button,
                                          int screenX, int screenY,
                                          int wheelDelta, uint32_t modifiers)
{
    // ⚠ 锁只罩住"路由决策"(命中/改 z 序/拖拽状态), **绝不罩住投递给客户区那一步**:
    //   窗口可能在别的线程上 (JZVirtualProcess), 投递会阻塞等它处理 —— 如果这时还握着
    //   桌面的锁, 而那个线程在处理过程中又要调桌面 (推帧/激活), 就是死锁。
    JZVirtualWindow* target = nullptr;      // 要在客户区里投递的窗口 (锁外投)
    int cx = 0, cy = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);

        // ---- 拖动/缩放进行中: 鼠标被窗口管理器抓走 (等价真系统的模态拖动循环) ----
        if (m_drag != DragNone && m_dragWin) {
            if (type == JZMouseEvent::Move) {
                doDrag(screenX, screenY);
                return true;
            }
            if (type == JZMouseEvent::Release) {
                endDrag();
                return true;
            }
        }

        // ---- 滚轮: 投给光标下窗口的客户区 (落在装饰上不投递) ----
        if (type == JZMouseEvent::Wheel) {
            JZVirtualWindow* w = windowAt(screenX, screenY);
            if (!w || w->hitTest(screenX, screenY) != JZVirtualWindow::HitClient)
                return false;
            const JZPoint p = screenToClient(w, screenX, screenY);
            target = w; cx = p.x(); cy = p.y();
        }
        else if (type == JZMouseEvent::Move) {
            return m_pressed != nullptr;      // 客户区里拖动中: 事件归我们
        }
        else if (type == JZMouseEvent::Press) {
            JZVirtualWindow* w = windowAt(screenX, screenY);
            if (!w)
                return false;                 // 点在桌面空白上: 不归任何窗口 (等价真实桌面)
            activate(w);                      // 点击激活窗口 (与真实桌面一致)

            switch (w->hitTest(screenX, screenY)) {
            case JZVirtualWindow::HitClose:
                destroyWindow(w);             // 点关闭 = 关掉这个窗口
                return true;
            case JZVirtualWindow::HitMinimize:
                w->setMinimized(!w->isMinimized());
                return true;
            case JZVirtualWindow::HitMaximize:
                w->setMaximized(!w->isMaximized());
                return true;
            case JZVirtualWindow::HitTitle:
                beginDragMove(w, screenX, screenY);
                return true;
            case JZVirtualWindow::HitBorderLeft:       beginDragResize(w, EdgeLeft, screenX, screenY); return true;
            case JZVirtualWindow::HitBorderRight:      beginDragResize(w, EdgeRight, screenX, screenY); return true;
            case JZVirtualWindow::HitBorderTop:        beginDragResize(w, EdgeTop, screenX, screenY); return true;
            case JZVirtualWindow::HitBorderBottom:     beginDragResize(w, EdgeBottom, screenX, screenY); return true;
            case JZVirtualWindow::HitBorderTopLeft:    beginDragResize(w, EdgeLeft | EdgeTop, screenX, screenY); return true;
            case JZVirtualWindow::HitBorderTopRight:   beginDragResize(w, EdgeRight | EdgeTop, screenX, screenY); return true;
            case JZVirtualWindow::HitBorderBottomLeft: beginDragResize(w, EdgeLeft | EdgeBottom, screenX, screenY); return true;
            case JZVirtualWindow::HitBorderBottomRight:beginDragResize(w, EdgeRight | EdgeBottom, screenX, screenY); return true;
            case JZVirtualWindow::HitClient: {
                // 客户区: 换算成客户区坐标投递给那个真 JZWindow (与被系统投递同一条路)
                const JZPoint p = screenToClient(w, screenX, screenY);
                m_pressed = w;
                target = w; cx = p.x(); cy = p.y();
                break;
            }
            default:
                return true;                  // 理论上到不了
            }
        }
        else if (type == JZMouseEvent::Release) {
            // ---- 松开: 还给按下时那个窗口 (它可能正抓取着鼠标拖拽) ----
            if (!m_pressed)
                return false;
            JZVirtualWindow* w = m_pressed;
            m_pressed = nullptr;
            const JZPoint p = screenToClient(w, screenX, screenY);
            target = w; cx = p.x(); cy = p.y();
        }
    }

    if (!target)
        return false;
    return target->injectMouse(type, button, cx, cy, wheelDelta, modifiers);   // 锁外投递
}

bool JZVirtualDevice::injectClickAt(int screenX, int screenY, JZMouseEvent::Button button)
{
    const bool down = injectMouseAtScreen(JZMouseEvent::Press, button, screenX, screenY);
    const bool up   = injectMouseAtScreen(JZMouseEvent::Release, button, screenX, screenY);
    return down || up;
}

bool JZVirtualDevice::injectKey(uint32_t key, const std::string& text,
                                uint32_t modifiers, bool press)
{
    JZVirtualWindow* w = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        w = m_active;
    }
    // 锁外投递 (同理: 可能阻塞在窗口的线程上, 那期间不许握着桌面的锁)
    return w ? w->injectKey(key, text, modifiers, press) : false;
}

bool JZVirtualDevice::injectText(const std::string& utf8)
{
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        if (!m_active)
            return false;
    }

    bool any = false;
    size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char c0 = (unsigned char)utf8[i];
        size_t len = 1;
        uint32_t cp = c0;
        if (c0 >= 0xF0)      { len = 4; cp = c0 & 0x07u; }
        else if (c0 >= 0xE0) { len = 3; cp = c0 & 0x0Fu; }
        else if (c0 >= 0xC0) { len = 2; cp = c0 & 0x1Fu; }
        if (i + len > utf8.size())
            len = 1;
        if (len > 1) {
            for (size_t k = 1; k < len; ++k)
                cp = (cp << 6) | ((unsigned char)utf8[i + k] & 0x3Fu);
        }
        const std::string ch = utf8.substr(i, len);
        injectKey(cp, ch, JZKeyEvent::NoModifier, true);
        injectKey(cp, ch, JZKeyEvent::NoModifier, false);
        any = true;
        i += len;
    }
    return any;
}

JZCursorShape JZVirtualDevice::cursorAt(int screenX, int screenY) const
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    JZVirtualWindow* w = windowAt(screenX, screenY);
    return w ? w->cursorShapeAt(screenX, screenY) : JZCursorShape::Arrow;
}

// ==================== 窗口管理器: 拖动 / 缩放 ====================
void JZVirtualDevice::beginDragMove(JZVirtualWindow* w, int x, int y)
{
    m_drag = DragMove;
    m_dragWin = w;
    m_dragDX = x - w->frameX();
    m_dragDY = y - w->frameY();
}

void JZVirtualDevice::beginDragResize(JZVirtualWindow* w, int edges, int x, int y)
{
    m_drag = DragResize;
    m_dragWin = w;
    m_dragEdges = edges;
    m_dragFrame = w->frameGeometry();
    m_dragOrigin = JZPoint(x, y);
}

void JZVirtualDevice::doDrag(int x, int y)
{
    if (!m_dragWin)
        return;

    if (m_drag == DragMove) {
        // 至少让标题栏留在屏内 (与真桌面一致: 不能把窗口拖到再也抓不回来)
        const int maxX = m_desktopW - 40;
        const int minX = 40 - m_dragWin->frameWidth();
        const int maxY = m_desktopH - JZVirtualWindow::kTitleBar;
        const int nx = clampInt(x - m_dragDX, minX, maxX);
        const int ny = clampInt(y - m_dragDY, 0, maxY);
        m_dragWin->setFramePos(nx, ny);       // 内部会通知桌面重新合成
        return;
    }

    // 缩放: 以按下时的外框为基准, 只动被抓住的边; 客户区尺寸有下限。
    const int dx = x - m_dragOrigin.x();
    const int dy = y - m_dragOrigin.y();
    int left = m_dragFrame.left();
    int top = m_dragFrame.top();
    int right = m_dragFrame.right() + 1;      // 半开区间, 便于加减
    int bottom = m_dragFrame.bottom() + 1;

    if (m_dragEdges & EdgeLeft)   left   += dx;
    if (m_dragEdges & EdgeRight)  right  += dx;
    if (m_dragEdges & EdgeTop)    top    += dy;
    if (m_dragEdges & EdgeBottom) bottom += dy;

    // 下限: 客户区不小于 JZVirtualWindow::kMinClient*
    const int minW = JZVirtualWindow::kMinClientW + 2 * JZVirtualWindow::kBorder;
    const int minH = JZVirtualWindow::kMinClientH + JZVirtualWindow::kTitleBar +
                     JZVirtualWindow::kBorder;
    if (right - left < minW) {
        if (m_dragEdges & EdgeLeft) left = right - minW;
        else                        right = left + minW;
    }
    if (bottom - top < minH) {
        if (m_dragEdges & EdgeTop) top = bottom - minH;
        else                       bottom = top + minH;
    }
    // 别把窗口缩到桌面外太多
    if (left < -m_desktopW) left = -m_desktopW;
    if (top < 0)            top = 0;

    m_dragWin->setFrameGeometry(JZRect(left, top, right - left, bottom - top));
}

void JZVirtualDevice::endDrag()
{
    m_drag = DragNone;
    m_dragWin = nullptr;
    m_dragEdges = 0;
}
