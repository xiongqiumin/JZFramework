// ==================== 虚拟桌面 (虚拟显示器 + 窗口管理器) ====================
// 分层: 桌面底色 + 窗口合成图 (画布 = 自己的离屏显示设备 m_canvas)
//          └─ JZVirtualWindow (外框: 标题栏 + 边框 + 三个按钮)
//               └─ 真 JZWindow (客户区: 控件树/渲染/输入)
//
// 坐标: 桌面坐标 == 屏幕**逻辑**坐标 (左上角原点; 缩放见 setScale, 它只决定渲染成多少
// 物理像素, 不进坐标)。窗口有自己的桌面坐标, 所以 screenToClient / clientToScreen /
// 命中测试 / 归一化 rect 全都有意义 —— 与真实桌面同构, CU 的"屏幕坐标 + 窗口"语义
// 可以直接落在这块虚拟桌面上。
#include "JZVirtualDevice.h"
#include "JZVirtualWindow.h"
#include "JZVirtualWindowBackend.h"   // 收窗口时要把客户区的后端换成它
#include "JZVirtualScreenView.h"
#include "JZPainter.h"
#include "JZWindow.h"
#include "JZDisplayDevice.h"
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

// ---- 任务栏 / 开始菜单布局 (逻辑像素; 随桌面缩放按物理分辨率渲染) ----
const int kStartBtnW = 58;
const int kTaskBtnW = 132;
const int kTaskGap = 3;
const int kTaskPad = 3;
const int kMenuW = 176;
const int kMenuItemH = 26;

// 经典 Windows 灰的立体观感 (凸: 左/上高光 + 右/下阴影)
const JZColor kBarFace(192, 192, 192, 255);
const JZColor kBarLight(255, 255, 255, 255);
const JZColor kBarShadow(112, 112, 112, 255);
const JZColor kBarText(24, 24, 24, 255);
const JZColor kBarWhite(255, 255, 255, 255);
const JZColor kBarActive(0x2b, 0x57, 0x9a, 255);   // 激活窗口按钮: 与标题栏同色

// 任务栏上"按 z 序可见的窗口" (按钮顺序、命中、激活都用它, 三处口径一致)
void visibleWindows(const std::vector<JZVirtualWindow*>& all, std::vector<JZVirtualWindow*>& out)
{
    out.clear();
    for (auto* w : all) {
        if (w && w->isVisible())
            out.push_back(w);
    }
}

} // namespace

JZVirtualDevice::JZVirtualDevice(int desktopW, int desktopH)
    : m_desktopBg(0, 128, 128, 255)      // 经典桌面绿 (Win95/98 那套青绿), 不是白屏
{
    m_desktopW = desktopW > 0 ? desktopW : 800;
    m_desktopH = desktopH > 0 ? desktopH : 600;
    ensureCanvas();      // 画布 = 桌面自己的离屏设备 (scale 默认 1 = 逻辑尺寸)
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
// 登记一个进程 (进程自己在 start 里调)。停/等的入口是 terminateProcess —— 桌面只认 JZThread。
// 进程**自己**结束 (未处理异常 -> JZVirtualProcess::run 末尾) 时也会来走 terminateProcess:
// 那条路是进程线程往本线程(主线程)投一个异步事件, 见 JZVirtualProcess::run。
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

    // ① 停线程 + 等它真正结束 —— 之后它再也不会碰桌面 (引擎也在那个线程里销毁了)。
    //    ⚠ 进程线程跑完时还会往本线程投一个"清理我"的事件 (见 JZVirtualProcess::run) ——
    //      那是个**不带发送者**的异步事件, 且捕获的是弱引用: 上面 join 之后本函数已经把 p
    //      删掉, 那个事件后到时判活失败直接跳过, 不会碰已失效的进程。
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
        // 桌面就是"屏幕": 底色必须不透明 —— 画布是后端原生缓冲, 每帧靠"铺底色"覆盖
        // 上一帧 (半透明底色会变成"叠在上一帧上", 越叠越脏)。
        m_desktopBg = JZColor(c.red(), c.green(), c.blue(), 255);
    }
    refresh();
}

void JZVirtualDevice::setView(JZVirtualScreenView* view)
{
    m_view = view;
}

// ==================== DPI (与主机共享缩放) ====================
// 只改"渲染成多少物理像素": 坐标仍是逻辑的, 命中/注入/归一化 rect 一个都不动。
// 窗口的帧缓冲尺寸与内容随缩放重建 => 这**是**一次全桌面重渲染, 只在 DPI 真变了时做。
void JZVirtualDevice::setScale(float scale)
{
    if (!(scale > 0.0f))
        scale = 1.0f;

    std::vector<JZVirtualWindow*> list;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        if (std::fabs(scale - m_scale) <= 0.001f)
            return;
        m_scale = scale;
        list = m_windows;      // 拷一份再到锁外干活 (下面会同步等窗口自己的线程重画)
    }

    // 画布自己也按新缩放重建帧缓冲 (它的显示位图随之刷新)
    ensureCanvas();

    // 每个窗口的客户区按新缩放重建帧缓冲并重画 —— 必须在**它自己的线程**上画
    // (applyDeviceScale 内部处理跨线程), 所以这一步不能握着桌面的锁。
    for (auto* w : list)
        w->applyDeviceScale();

    refresh();
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
    if (!m_view)
        return;               // 纯无头: 合成完就够 (取帧见 desktopImage()/screenshot())
    // 视图绘制时读 desktopPixmap(), 所以这里只需让它重绘 —— 但**怎么重绘**分两种:
    //   · 拖动/缩放窗口进行中: 同步重绘 (paintNow)。窗口管理器此刻就是鼠标的主人,
    //     用户要的是"窗口粘在光标上"; 而 update() 只是排个脏区, 要等消息循环下一轮
    //     (快速拖动时窗口明显慢半拍 —— 实测滞后 200ms 上下);
    //   · 其余情况: 常规 update(), 同一轮里的多次改动由消息循环合并成一帧。
    if (m_drag != DragNone)
        m_view->paintNow();
    else
        m_view->update();
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
    // 在**工作区**里居中 (任务栏那一条不摆窗口, 免得一上来就被任务栏压住/压住任务栏)
    const int areaH = taskbarTop();
    int x = (m_desktopW - fw) / 2 + off;
    int y = (areaH - fh) / 2 + off;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    return JZPoint(x, y);
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
    JZVirtualWindow* old = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);   // 进程线程也会调 (show 里)
        raise(w);
        if (m_active == w && w->isActive())
            return;                                          // 已经是活动且已聚焦: 没事可做
        old = (m_active == w) ? nullptr : m_active;
        m_active = w;
    }

    // ⚠ 聚焦是**跨线程**的: client()->setWindowFocus -> onWindowFocus -> sendEvent 会投递到
    //   窗口自己的线程并**等它处理** —— 所以必须锁外做。握着锁等别的线程时, 那个线程可能
    //   正回头抢这把锁 (推帧/它自己 show 里激活), 于是双方互等: 死锁。
    //   (实测复现: 进程窗口 show 时它自己调 activate, 同时主线程正点它 —— 必卡死。)
    if (old)
        old->setFocus(false);        // 旧活动窗口失焦 (派发 focusOut, 取消未上屏的编辑)
    w->setFocus(true);
    refresh();
}

void JZVirtualDevice::moveWindow(JZVirtualWindow* w, int x, int y)
{
    if (w)
        w->setFramePos(x, y);         // 窗口自己会通知桌面重新合成
}

// 窗口表的**快照** (锁内拷一份, z 序不变)。
// 为什么要有它: windows() 返回的是那张活表 —— 进程线程建窗 (adoptWindow) / 客户区被 GC 都会
// 动它, 跨线程或"先查后用"的调用方一旦直接遍历就是在读一份随时会变的数据。要拷出来就用这个。
std::vector<JZVirtualWindow*> JZVirtualDevice::windowList() const
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    return m_windows;
}

// 某个进程建的窗口 (z 序, 由下到上)。p 传 nullptr = 宿主自己 (同线程) 建的那些窗口 ——
// "查一个程序在桌面上开了哪些窗口" 就靠它 (进程对桌面来说只是 JZThread, 见类头注释)。
std::vector<JZVirtualWindow*> JZVirtualDevice::windowsOfProcess(JZThread* p) const
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    std::vector<JZVirtualWindow*> out;
    for (auto* w : m_windows) {
        if (w->process() == p)
            out.push_back(w);
    }
    return out;
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
    if (screenY >= taskbarTop())
        return nullptr;      // 任务栏那一条是桌面 chrome: 那里没有窗口 (视觉上也是它盖着)
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

// ==================== 桌面 chrome: 任务栏 + 开始菜单 ====================
// 任务栏是"屏幕底下的一条", 由桌面自己画 (不是窗口) —— 于是它天然压在窗口之上, 且
// 最大化只铺到 workArea() (见 JZVirtualWindow::setMaximized), 不会盖住它。
// 几何只有这一份: 绘制与命中都调下面这几个 rect。
JZRect JZVirtualDevice::startButtonRect() const
{
    return JZRect(kTaskPad, taskbarTop() + kTaskPad, kStartBtnW, kTaskbarHeight - 2 * kTaskPad);
}

JZRect JZVirtualDevice::taskButtonRect(int index) const
{
    const int x = kTaskPad + kStartBtnW + kTaskGap + (kTaskBtnW + kTaskGap) * index;
    return JZRect(x, taskbarTop() + kTaskPad, kTaskBtnW, kTaskbarHeight - 2 * kTaskPad);
}

JZRect JZVirtualDevice::startMenuRect() const
{
    const int h = (int)m_startItems.size() * kMenuItemH + 2 * kTaskPad;
    int y = taskbarTop() - h - kTaskGap;         // 从开始按钮往**上**弹
    if (y < 0)
        y = 0;
    return JZRect(kTaskPad, y, kMenuW, h);
}

JZRect JZVirtualDevice::startMenuItemRect(int item) const
{
    const JZRect m = startMenuRect();
    return JZRect(m.left() + kTaskPad, m.top() + kTaskPad + item * kMenuItemH,
                  m.width() - 2 * kTaskPad, kMenuItemH);
}

int JZVirtualDevice::taskButtonAt(int x, int y) const
{
    if (y < taskbarTop())
        return -1;
    std::vector<JZVirtualWindow*> list;
    visibleWindows(m_windows, list);
    for (int i = 0; i < (int)list.size(); ++i) {
        if (taskButtonRect(i).contains(x, y))
            return i;
    }
    return -1;
}

int JZVirtualDevice::startMenuItemAt(int x, int y) const
{
    if (!m_startMenuOpen)
        return -1;
    for (int i = 0; i < (int)m_startItems.size(); ++i) {
        if (startMenuItemRect(i).contains(x, y))
            return i;
    }
    return -1;
}

int JZVirtualDevice::addStartMenuItem(const std::string& text, std::function<void()> onActivate)
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    StartMenuItem it;
    it.text = text;
    it.onActivate = std::move(onActivate);
    m_startItems.push_back(std::move(it));
    return (int)m_startItems.size() - 1;
}

void JZVirtualDevice::setStartMenuOpen(bool on)
{
    if (m_startMenuOpen == on)
        return;
    m_startMenuOpen = on;
    if (!on)
        m_hoverStartItem = -1;
    refresh();
}

// 任务栏/开始菜单的点击: 返回 true = 这次点击归桌面 (不再往下投给窗口);
// pending 带出"需要锁外执行"的菜单项回调 (见 injectMouseAtScreen 的注释)。
bool JZVirtualDevice::handleChromePress(int x, int y, std::function<void()>& pending)
{
    const bool inBar = (y >= taskbarTop() && y < m_desktopH + 1);

    // 开始菜单开着: 点菜单项 = 执行; 点别处 = 收起 (这一下**不**落到下面, 同真实开始菜单)
    if (m_startMenuOpen) {
        const int item = startMenuItemAt(x, y);
        if (item >= 0) {
            pending = m_startItems[item].onActivate;
            setStartMenuOpen(false);
            return true;
        }
        setStartMenuOpen(false);
        return true;
    }

    if (inBar) {
        if (startButtonRect().contains(x, y)) {
            setStartMenuOpen(true);
            return true;
        }
        const int b = taskButtonAt(x, y);
        if (b >= 0) {
            activateTaskButton(b);           // 点窗口按钮 = 激活/还原那个窗口
            return true;
        }
        return true;                         // 任务栏空白: 吞掉
    }
    return false;
}

// 悬停反馈 (开始按钮/窗口按钮/菜单项高亮); 返回 true = 光标在 chrome 上
bool JZVirtualDevice::updateChromeHover(int x, int y)
{
    const int item = startMenuItemAt(x, y);
    int task = -2;                           // -2 = 无高亮
    if (y >= taskbarTop()) {
        if (startButtonRect().contains(x, y))
            task = -1;
        else
            task = taskButtonAt(x, y);
    }
    const bool onChrome = (y >= taskbarTop()) || (item >= 0);
    if (item != m_hoverStartItem || task != m_hoverTask) {
        m_hoverStartItem = item;
        m_hoverTask = task;
        refresh();                           // 高亮变了: 重新合成 (普通 update 即可)
    }
    return onChrome;
}

void JZVirtualDevice::activateTaskButton(int index)
{
    std::vector<JZVirtualWindow*> list;
    visibleWindows(m_windows, list);
    if (index < 0 || index >= (int)list.size())
        return;
    JZVirtualWindow* w = list[index];
    if (w->isMinimized())
        w->setMinimized(false);              // 最小化的窗口: 点任务栏按钮 = 还原
    activate(w);
}

void JZVirtualDevice::paintTaskbar(JZPainter& p)
{
    const int top = taskbarTop();
    p.fillRect(0, top, m_desktopW, kTaskbarHeight, kBarFace);
    p.fillRect(0, top, m_desktopW, 1, kBarLight);          // 上沿高光

    // 开始按钮 (按下/悬停时凹下去, 与经典任务栏一致)
    {
        const JZRect r = startButtonRect();
        const bool down = m_startMenuOpen || m_hoverTask == -1;
        p.fillRect(r, down ? kBarShadow : kBarFace);
        p.fillRect(r.left(), r.top(), r.width(), 1, down ? kBarShadow : kBarLight);
        p.fillRect(r.left(), r.bottom(), r.width(), 1, down ? kBarLight : kBarShadow);
        p.setFont(JZFont(12));
        p.setPen(JZPen(down ? kBarWhite : kBarText, 1));
        p.drawText(r.left() + 14, r.top() + 5, "开始");
    }

    // 每个可见窗口一个按钮 (点它 = 激活; 活动窗口那个是"按住"的样子)
    std::vector<JZVirtualWindow*> list;
    visibleWindows(m_windows, list);
    p.setFont(JZFont(12));
    for (int i = 0; i < (int)list.size(); ++i) {
        const JZRect r = taskButtonRect(i);
        if (r.right() >= m_desktopW - kTaskPad)
            break;                                          // 摆不下了 (简单的截断)
        const bool active = (list[i] == m_active);
        const bool down = active || m_hoverTask == i;
        p.fillRect(r, down ? kBarShadow : kBarFace);
        p.fillRect(r.left(), r.top(), r.width(), 1, down ? kBarShadow : kBarLight);
        p.fillRect(r.left(), r.bottom(), r.width(), 1, down ? kBarLight : kBarShadow);
        p.setPen(JZPen(active ? kBarWhite : kBarText, 1));
        // ⚠ 本框架的 save/restore 只管 pen/brush/font/matrix, **不含裁剪** (见 JZPainter 注释):
        //   这里为了截断标题临时收窄裁剪, 画完必须显式还原成整块画布, 否则后面的开始菜单
        //   会被裁在这颗按钮里 (踩过)。
        p.setClip(JZRect(r.left() + 1, r.top() + 1, r.width() - 2, r.height() - 2));
        p.drawText(r.left() + 6, r.top() + 5, list[i]->title().c_str());
        p.setClip(JZRect(0, 0, m_desktopW, m_desktopH));
    }
}

void JZVirtualDevice::paintStartMenu(JZPainter& p)
{
    if (m_startItems.empty())
        return;
    const JZRect m = startMenuRect();
    p.fillRect(m, kBarFace);
    p.fillRect(m.left(), m.top(), m.width(), 1, kBarLight);          // 立体边框
    p.fillRect(m.left(), m.top(), 1, m.height(), kBarLight);
    p.fillRect(m.left(), m.bottom(), m.width(), 1, kBarShadow);
    p.fillRect(m.right(), m.top(), 1, m.height() - 1, kBarShadow);

    p.setFont(JZFont(12));
    for (int i = 0; i < (int)m_startItems.size(); ++i) {
        const JZRect r = startMenuItemRect(i);
        const bool hot = (m_hoverStartItem == i);
        if (hot)
            p.fillRect(r, kBarActive);                               // 悬停: 与标题栏同色的高亮
        p.setPen(JZPen(hot ? kBarWhite : kBarText, 1));
        p.drawText(r.left() + 10, r.top() + 6, m_startItems[i].text.c_str());
    }
}

// ==================== 合成 / 取帧 ====================
// 画布 = 本桌面自己的一块**离屏显示设备**: 后端把外框与客户区帧直接画进它的帧缓冲
// (GDI+ 的原生布局)。于是"画"和"贴"之间没有任何 RGBA<->PARGB 的全图转换 ——
// 显示控件拿到的是画布缓冲的零拷贝位图 (见 desktopPixmap)。
// 尺寸/缩放变了才重建缓冲 (拖动时零分配、零转换)。
void JZVirtualDevice::ensureCanvas()
{
    m_canvas.setScale(m_scale);          // 先定缩放 (物理尺寸换算与它一致)
    const int iw = toPhysical(m_desktopW);
    const int ih = toPhysical(m_desktopH);
    if (!m_canvasReady) {
        m_canvas.initOffscreen(iw, ih);  // 首次: 自建与窗口无关的兼容 DC/DIB
        m_canvasReady = true;
    } else {
        m_canvas.resize(iw, ih);         // 尺寸没变时后端内部直接返回 (零开销)
    }

    // 画布缓冲的零拷贝显示位图 (缓冲重建后要重新取一次; 同一缓冲时原样复用)
    JZPixmap pm;
    if (m_canvas.targetPixmap(pm))
        m_desktopPix = pm;
}

void JZVirtualDevice::compose()
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);   // 与进程线程的建窗/推帧互斥
    if (m_composing)
        return;
    m_composing = true;

    pruneWindows();     // 客户区已被回收的窗口先从表里摘掉 (之后再遍历才安全)
    ensureCanvas();     // 画布缓冲 (尺寸/缩放变了才重建)

    {
        // 一块画布一个 painter: 底色 + 外框 + 客户区帧按 z 序由下到上叠, 析构时一次性
        // 回放到画布的帧缓冲 (原生布局, 不做 RGBA 往返)。
        // ⚠ 坐标是**逻辑**的 (桌面坐标), 物理换算由画布设备的缩放施加 ——
        //   "坐标与 DPI 无关, 只有渲染分辨率随 DPI 变"这条规矩就落在这一句上。
        JZPainter p(&m_canvas, JZRect(0, 0, m_desktopW, m_desktopH));
        // 每帧先铺底色 (窗口可能被移走/关掉了, 旧像素必须被盖掉)。底色必须不透明,
        // 否则这里是"叠"上一帧而不是覆盖它 (见 setDesktopBackground)。
        p.fillRect(0, 0, m_desktopW, m_desktopH, m_desktopBg);

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
            // 帧是物理像素 (客户区逻辑尺寸 * scale), 这里给的是**逻辑**矩形 ⇒ 1:1 落上去。
            const JZImage& f = w->frame();
            if (f.isNull() || !f.data())
                continue;
            const JZRect c = w->clientGeometry();
            p.drawImage(f, c.left(), c.top(), c.width(), c.height());
        }

        // 桌面 chrome: 任务栏 + (已弹出的话) 开始菜单 —— 最后画, 所以恒在窗口之上
        paintTaskbar(p);
        if (m_startMenuOpen)
            paintStartMenu(p);
    }

    ++m_frameSerial;      // 合成图变了 (见 frameSerial)
    m_composing = false;
}

// 整屏一帧 (RGBA8888; **物理像素**: 桌面逻辑尺寸 * scale; 无头默认 scale=1 即逻辑尺寸)。
// ⚠ 这是"画布 -> RGBA"的一次按需转换 (O(像素)), 只在截图/CU/测试这条路上发生;
//   显示控件走的是 desktopPixmap() 的零拷贝位图, 拖动时不会碰这里。
const JZImage& JZVirtualDevice::desktopImage()
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    if (m_imageSerial != m_frameSerial || m_desktopImage.isNull()) {
        ensureCanvas();
        if (!m_canvas.snapshot(m_desktopImage))
            m_desktopImage = JZImage();
        m_imageSerial = m_frameSerial;
    }
    return m_desktopImage;
}

JZImage JZVirtualDevice::screenshot()
{
    refresh();
    return desktopImage();               // 拷贝很轻 (像素缓冲共享)
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
    JZVirtualWindow* activateTarget = nullptr;   // 需要激活的窗口: 也锁外做 (见 activate)
    std::function<void()> chromeAction;     // 菜单项回调: 一律锁外跑 (见 handleChromePress)
    bool chromeHandled = false;             // 这次事件被桌面 chrome (任务栏/开始菜单) 吃掉
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
            if (m_pressed)
                return true;                  // 客户区里拖动中: 事件归我们 (含拖出客户区)
            if (updateChromeHover(screenX, screenY))
                return true;                  // 光标在任务栏/开始菜单上: 不打给窗口
            return false;
        }
        else if (type == JZMouseEvent::Press) {
            // 桌面 chrome (任务栏/开始菜单) 恒在窗口之上, 先问它
            if (handleChromePress(screenX, screenY, chromeAction)) {
                chromeHandled = true;
            } else {
                JZVirtualWindow* w = windowAt(screenX, screenY);
                if (!w)
                    return false;             // 点在桌面空白上: 不归任何窗口 (等价真实桌面)
                activateTarget = w;           // 点击激活窗口 (与真实桌面一致): ⚠ 锁外激活

                switch (w->hitTest(screenX, screenY)) {
                case JZVirtualWindow::HitClose:
                    destroyWindow(w);         // 点关闭 = 关掉这个窗口
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
                    return true;              // 理论上到不了
                }
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

    // ---- 锁外这一步, 三件事的顺序与"真桌面 + 真系统"一致: 先激活, 再执行 chrome 动作,
    //      最后把事件投给客户区。⚠ 三者**都**不能握着锁做 (见 activate / 下面的注释)。 ----
    if (activateTarget)
        activate(activateTarget);

    // 开始菜单项的回调 (建窗/起进程/停进程) 一律**锁外**执行 —— 回调里可能要等新线程
    // (如 waitWindow), 而那个线程回头就要抢 m_mutex, 握着锁等它就是死锁。
    if (chromeAction)
        chromeAction();
    if (chromeHandled)
        return true;

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
    JZVirtualWindow* w = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        w = m_active;
    }
    // 逐字符那套实现只有一份, 在窗口上 (JZVirtualWindow::injectText) —— 锁外调用
    return w ? w->injectText(utf8) : false;
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

// ==================== 程序输出的环形缓冲 (见 JZVirtualDevice.h) ====================
// 只保留最近 kOutputLines 行; 行号(绝对) = 一进来就 +1, 于是"被挤掉多少"能从 total 与
// 现有条数之差算出来 (见 outputFirst) —— 消费方据此做增量拉取。
void JZVirtualDevice::appendOutput(const std::string& line)
{
    std::lock_guard<std::mutex> lock(m_outputMutex);
    m_outputs.push_back(line);
    m_outputTotal++;
    while ((int)m_outputs.size() > kOutputLines)
        m_outputs.pop_front();
}

int JZVirtualDevice::outputTotal() const
{
    std::lock_guard<std::mutex> lock(m_outputMutex);
    return (int)m_outputTotal;
}

int JZVirtualDevice::outputFirst() const
{
    std::lock_guard<std::mutex> lock(m_outputMutex);
    return (int)(m_outputTotal - (long long)m_outputs.size());   // 已被挤掉的行数 == 现存第一行的行号
}

std::vector<std::string> JZVirtualDevice::outputLines(int offset, int size) const
{
    std::lock_guard<std::mutex> lock(m_outputMutex);
    std::vector<std::string> out;
    const long long first = m_outputTotal - (long long)m_outputs.size();
    long long from = (offset > 0) ? offset : 0;
    if (from < first)
        from = first;                     // 给得太早(或默认 0): 从最老的还留着的那行开始
    if (from >= m_outputTotal)
        return out;                       // 给得太晚: 这些行已经滚掉了
    size_t begin = (size_t)(from - first);
    size_t end = m_outputs.size();
    if (size > 0 && (size_t)size < end - begin)
        end = begin + (size_t)size;
    out.reserve(end - begin);
    for (size_t i = begin; i < end; i++)
        out.push_back(m_outputs[i]);
    return out;
}
