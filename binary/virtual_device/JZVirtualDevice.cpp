// ==================== 虚拟桌面 (虚拟显示器 + 窗口管理器) ====================
// 分层: 桌面底色 + 窗口合成图 (画布 = 自己的离屏显示设备 m_canvas)
//          └─ JZVirtualWindow (外框: 标题栏 + 边框 + 三个按钮)
//               └─ 真 JZWindow (客户区: 控件树/渲染/输入)
//
// 坐标: 桌面坐标 == 屏幕**逻辑**坐标 (左上角原点; 缩放见 setScale, 它只决定渲染成多少
// 物理像素; 对外接口 (HTTP/CU/截图) 一律物理)。窗口有自己的桌面坐标, 所以 screenToClient / clientToScreen /
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
#include "JZDateTime.h"      // 刷新节流 (见 refresh)
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

// 任务栏上"按 z 序可见的窗口" (命中/枚举都用它)
void visibleWindows(const std::vector<JZVirtualWindow*>& all, std::vector<JZVirtualWindow*>& out)
{
    out.clear();
    for (auto* w : all) {
        if (w && w->isVisible())
            out.push_back(w);
    }
}

// 任务栏**按钮的排列顺序** = 窗口诞生顺序 (== 真实 Windows 的任务栏: 按钮顺序是稳的)。
// ⚠ 不能拿 z 序 (visibleWindows 的天然顺序) 排按钮: activate 会把窗口挪到 z 序最上
//   (见 raise), 于是点一下按钮 -> 这个按钮连同后面的全换位置 -> 再点就点到别的窗口了
//   ("窗口排序会切换" 的根因)。z 序是"谁盖谁", 按钮顺序是"谁先来的", 两件事。
void taskbarWindows(const std::vector<JZVirtualWindow*>& all, std::vector<JZVirtualWindow*>& out)
{
    visibleWindows(all, out);
    std::stable_sort(out.begin(), out.end(), [](JZVirtualWindow* a, JZVirtualWindow* b) {
        return a->creationSeq() < b->creationSeq();
    });
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

    // ③ 桌面持有窗口 (它们没有别的父对象): 桌面上的 + 已关闭(摘下来)但客户区还活着的一起收
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    std::vector<JZVirtualWindow*> list = m_windows;
    list.insert(list.end(), m_detached.begin(), m_detached.end());
    m_windows.clear();
    m_detached.clear();
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

    // ② 收掉它建的窗口 (客户区可能已被 JS 侧回收, destroyWindow/prune 都能处理) ——
    //    两张表都要扫: 它可能已经被"点关闭"摘到 m_detached 里了 (客户区还活着)。
    std::vector<JZVirtualWindow*> mine;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        for (auto* w : m_windows) {
            if (w->process() == p)
                mine.push_back(w);
        }
        for (auto* w : m_detached) {
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
// 显式设置 (用户/接口): 这块虚拟屏的 DPI 从此不跟宿主走 (见头文件)
void JZVirtualDevice::setScale(float scale)
{
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        if (m_scaleExplicit && std::fabs(scale - m_scale) <= 0.001f) {
            m_scaleExplicit = true;      // 同值也是"显式"(可能刚从跟随切过来): 标志要立起来
            return;
        }
        // ⚠ 标志必须在 early-return 之前定, 否则"显式设了和当前一样的值"会被当成跟随。
        m_scaleExplicit = true;
    }
    setScaleValue(scale);
}

// 宿主同步 (显示控件调): 只有没被显式设置时才生效 —— "默认与主机同 DPI"就落在这两句上
void JZVirtualDevice::setHostScale(float scale)
{
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        if (m_scaleExplicit)
            return;
    }
    setScaleValue(scale);
}

// 恢复"跟主机同 DPI": 清掉显式标志, 并**立刻**回到宿主当前的缩放
// (无头没有宿主窗口 ⇒ 1.0 = 纯逻辑桌面)。这一步不能只清标志 —— 那样 scale 会停在
// 上一个显式值上, 直到宿主 DPI 恰好变一次才纠正 (实测: 无头下永远停在旧值)。
void JZVirtualDevice::followHostScale()
{
    float host = 1.0f;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        m_scaleExplicit = false;
        if (m_view) {
            JZWindow* hw = m_view->window();
            if (hw && hw->device())
                host = hw->device()->scale();
        }
    }
    setScaleValue(host);       // 锁外: 它会同步等各窗口自己的线程重画
}

// 真正换缩放: 画布与每个窗口客户区都按新缩放重建帧缓冲并重画
void JZVirtualDevice::setScaleValue(float scale)
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
// 别的线程(进程线程)改了桌面时, 把刷新**转投**回主线程。
void JZVirtualDevice::refresh()
{
    JZThread* main = jzApp ? jzApp->mainThread() : nullptr;
    if (main && JZThread::currentThread() != main)
    {
        // ⚠ 这条路径**必须无锁**: 进程线程推帧/改窗口状态时, 主线程可能正卡在
        //   sendEvent 等它 (输入投递) —— 再去抢 m_mutex 就是死锁。
        // ⚠ 这里按**时间节流**, 而不是"已排了一帧"那种**永久**去重:
        //   · 永久去重只要漏掉一次投递, 就把之后**所有**刷新吞掉 —— 窗口帧一直在变、合成却
        //     再也不跑, 桌面缓存冻在最初那一帧(实测: 虚拟进程里的脚本一直看到启动那幅空客户区);
        //   · 完全不合并又会把主线程压垮 —— 合成是 O(像素) 的重活, 而窗口帧变化/取图请求会很密
        //     (实测: 脚本一拍问几次颜色, 合成队列越积越长, 一拍从 0.6s 涨到 6s、画面还冻着)。
        //   50ms ≈ 20 帧/秒: 桌面对外那一帧不需要跟上窗口自己的 60+ 帧 —— 合成本身是
        //   O(像素) 的重活(GDI+ 重放所有窗口 + 任务栏 ~18ms), 频率给高了主线程就饱和:
        //   实测 30 次/秒时占用过半, 合成排队 ⇒ 取图方(虚拟进程里的脚本)反而"好几分钟里
        //   有 8 拍看不见自己", 一局白打(score=0)。20 帧对"看得见、判得准"绰绰有余。
        //   ⚠ 拖动/缩放窗口走的是 refreshNow 的 paintNow 那条路, 不受这里的节流影响。
        const int64_t now = JZDateTime::currentMSecsSinceEpoch();
        if (now - m_lastRefreshReqMs < 50)
            return;
        m_lastRefreshReqMs = now;
        runOnObject(this, [this]() {              // 对象没了就不会跑 (守卫)
            refreshNow();
        });
        return;
    }
    refreshNow();
}

void JZVirtualDevice::refreshNow()
{
    compose();
    // 合成完**顺手把对外那一帧(RGBA)也快照好** —— 快照必须由画布的主人(就是这里)做:
    // 让"谁调用谁顺手做"的话, 非主人的线程(虚拟进程里的应用)做这件事会失败并留下一张
    // **空图**, 还把 serial 对齐(见 desktopImage 的说明) ⇒ 对外那帧被钉死, 那些应用
    // 在画面明明已经画出来之后还"什么都看不见"(实测: 脚本前 5 秒一片空白, 直到某次
    // 主线程取图才把缓存救回来 —— 表现为画面滞后好几秒)。
    // 代价: 一次 O(像素) 的转换 (800x600 ≈ 1.9MB); 有显示控件时贴的是 desktopPixmap()
    //       的零拷贝位图, 本来就不走这条路。
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        if (m_canvas.snapshot(m_desktopImage))
            m_imageSerial = m_frameSerial;
    }
    if (!m_view)
        return;               // 纯无头: 合成 + 对外那帧都好了 (取帧见 desktopImage()/screenshot())
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
// 两张表都扫: 桌面上的 (m_windows) 与"已关闭但客户区还在"的 (m_detached) —— 后者等客户区死了
// 才轮到我们把外框收掉 (它活着时不能删: 客户区的平台后端还指着它)。
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

    for (size_t i = 0; i < m_detached.size(); ) {
        JZVirtualWindow* w = m_detached[i];
        if (w->isClientAlive()) {
            ++i;
            continue;
        }
        m_detached.erase(m_detached.begin() + (long)i);
        delete w;                    // 同上: 客户区已死, 只收外框
    }
}

// 把 w 从两张表里摘下来 (锁内) + 维护活动/按下/拖动现场。返回"焦点该交给谁"(锁外再调)。
JZVirtualWindow* JZVirtualDevice::removeWindowLocked(JZVirtualWindow* w)
{
    JZVirtualWindow* focusNext = nullptr;
    auto it = std::find(m_windows.begin(), m_windows.end(), w);
    if (it != m_windows.end())
        m_windows.erase(it);
    else {
        auto dit = std::find(m_detached.begin(), m_detached.end(), w);
        if (dit != m_detached.end())
            m_detached.erase(dit);
    }
    if (m_pressed == w)
        m_pressed = nullptr;
    if (m_dragWin == w)
        endDrag();
    if (m_active == w) {
        // 活动窗口没了: 退到最上层那个 (等价桌面行为)
        m_active = m_windows.empty() ? nullptr : m_windows.back();
        focusNext = m_active;
    }
    return focusNext;
}

// "销毁平台窗口" (== 客户区 close() 被接受): 从桌面摘下来, **对象都留着** —— 客户区归 JS,
// 它的平台后端指着这台外框, 所以外框不能删 (删了就是野指针)。收尾交给 pruneWindows。
void JZVirtualDevice::detachWindow(JZVirtualWindow* w)
{
    if (!w)
        return;
    JZVirtualWindow* focusNext = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        auto it = std::find(m_windows.begin(), m_windows.end(), w);
        if (it == m_windows.end())
            return;                  // 已经不在桌面上了 (重复关闭/已被收走): 不重复搬
        focusNext = removeWindowLocked(w);
        m_detached.push_back(w);
    }
    // 锁外通知 (同 destroyWindow 那条纪律)
    if (focusNext)
        focusNext->setFocus(true);
    refresh();
}

// "重新显示" (== 重建 HWND): 之后客户区的 show() 会走到这里, 把同一台外框挂回桌面 (幂等)。
void JZVirtualDevice::attachWindow(JZVirtualWindow* w)
{
    if (!w)
        return;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        auto dit = std::find(m_detached.begin(), m_detached.end(), w);
        if (dit == m_detached.end())
            return;                  // 本来就在桌面上 (或已被收走): 什么都不做
        m_detached.erase(dit);
        m_windows.push_back(w);      // z 序最上 (之后的 activate 会正襟危坐地置顶+聚焦)
    }
    refresh();
}

bool JZVirtualDevice::hasWindow(const JZVirtualWindow* w) const
{
    if (!w)
        return false;
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    return std::find(m_windows.begin(), m_windows.end(), w) != m_windows.end();
}

void JZVirtualDevice::destroyWindow(JZVirtualWindow* w)
{
    if (!w)
        return;

    // ⚠ 先确认它还在两张表里, **之后**才解引用: 本函数也会被"迟到"的跨线程投递调到
    //   (见下面那段), 那时 w 可能已经被收走了 —— 表里找不到就直接当"已经没了"。
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        if (std::find(m_windows.begin(), m_windows.end(), w) == m_windows.end() &&
            std::find(m_detached.begin(), m_detached.end(), w) == m_detached.end())
            return;
    }

    // 窗口属于**别的线程** (虚拟进程里 JS 建出来的): 客户区是那个线程里的对象, 只有它自己的
    // 线程能销毁它 —— 本线程 delete 会踩 JZObject::clearGuardPointer 的线程亲和断言 (实测:
    // 主线程点进程窗口的关闭按钮, 主线程 delete 进程线程的 JZWindow -> 断言, 桌面状态跟着坏掉)。
    // 所以把"收摊"整件事**投到窗口自己的线程**做, 而且**只投不等**:
    //   ① 客户区的析构里还会有别的跨线程动作 (信号/焦点/布局), 在这边同步等它就是又制造一个
    //      "两个线程互相等对方处理自己投出去的事件" 的机会 —— 这一类互等在本项目已经踩过几次
    //      (见 activate / setFocus 上的注释);
    //   ② 收摊是**收尾**动作, 调用方 (terminateProcess / 容器析构) 不靠它的返回值。
    //   代价: 这条路是**异步**的 —— 返回时窗口可能还在表里, 那一投处理完才没。
    //   同线程窗口 (宿主自己建的) 不受影响: 照旧同步销毁。
    if (JZThread* owner = w->process()) {
        if (owner != JZThread::currentThread() && owner->isRunning()) {
            JZObjectPointer deskGuard = guardPointer();   // 投递后本桌面可能先没了: 判活再动
            runOnThread(owner, [this, w, deskGuard]() {
                if (deskGuard.object())
                    destroyWindow(w);
            });
            return;
        }
    }

    JZVirtualWindow* focusNext = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        // 摘之前再确认一次 (与上面那次检查同锁域): 已经被别的路径收走的话, 这里再 delete 就是双删。
        const bool inMain = std::find(m_windows.begin(), m_windows.end(), w) != m_windows.end();
        const bool inDet = !inMain &&
            std::find(m_detached.begin(), m_detached.end(), w) != m_detached.end();
        if (!inMain && !inDet)
            return;
        focusNext = removeWindowLocked(w);
    }

    // ⚠ 下面两件都在**锁外**做 (同 activate 那条纪律): 表里已经摘掉了, 没有别人能再拿到它;
    //   而"给别的窗口设焦点"与"客户区的析构"都可能是跨线程动作 —— 握着 m_mutex 等别的线程
    //   就是死锁配方 (那个线程回头也要用这把锁)。
    if (focusNext)
        focusNext->setFocus(true);
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

    // 桌面状态的改动在锁内做完, **通知**(焦点/重画) 一律锁外 —— 同 destroyWindow 那条纪律。
    // 焦点这条现在是安全的: JZVirtualWindow::setFocus 对**别的线程**的客户区是异步投递的
    // (见那里的说明), 不会握着锁等在别的线程上。
    // 历史: 它以前是同步 sendEvent, 于是"进程窗口 show 时它自己调 activate" 与 "主线程正
    //   给这个进程注入输入" 会互等 —— 双方各等对方处理自己投出去的那一条 (实测必卡死)。
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
    taskbarWindows(m_windows, list);
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
    taskbarWindows(m_windows, list);
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

    // 每个可见窗口一个按钮 (点它 = 激活; 活动窗口那个是"按住"的样子; 顺序见 taskbarWindows)
    std::vector<JZVirtualWindow*> list;
    taskbarWindows(m_windows, list);
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
            // 最小化: 桌面上什么都不画 —— 它只剩任务栏那颗按钮 (底下的标题栏按钮/任务栏
            // 里点它都能还原; 同真实 Windows, 最小化不是"缩小成一个标题栏")。
            if (w->isMinimized())
                continue;
            // 外框 (标题栏/边框/按钮) —— 窗口管理器那半边
            w->paintDecoration(p, w == m_active);
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
// 缓存由**合成之后**的 refreshNow 刷新 (画布主人做快照), 这里只读 —— 与"窗口帧缓存"
// (JZVirtualWindow::m_frame, 由窗口线程画完后拷好) 同一套形状: 谁的东西谁维护, 别人只读。
// ⚠ 别让"调用 desktopImage 的线程"顺手做快照: 非画布主人的线程做会失败并留下一张**空图**,
//   还把 serial 对齐(m_imageSerial = m_frameSerial) ⇒ 这一帧被钉死, 之后谁都拿不到新画布。
//   实测就是虚拟进程里的脚本"画面明明有了却看不见", 要等某次主线程取图才恢复。
const JZImage& JZVirtualDevice::desktopImage()
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    if (m_desktopImage.isNull() || m_imageSerial != m_frameSerial) {
        // 正常路径走不到这儿 (合成时会一起快照好)。只有两种情况:
        //   · 还从没合成过 (缓存空) —— 那就等下一次合成;
        //   · 调用方**就是**画布主人 (宿主里的显示控件/主线程) —— 补一次也无妨。
        JZThread* main = jzApp ? jzApp->mainThread() : nullptr;
        if (!main || JZThread::currentThread() == main) {
            ensureCanvas();
            if (!m_canvas.snapshot(m_desktopImage))
                m_desktopImage = JZImage();
            m_imageSerial = m_frameSerial;
        }
    }
    return m_desktopImage;
}

JZImage JZVirtualDevice::screenshot()
{
    // ⚠ 这里**不跨线程等**主线程合成: 窗口线程(虚拟进程)调进来时如果同步等主线程,
    //   而主线程此刻可能正等着这个窗口线程(抓窗口帧 / 输入投递), 就成了互相等 ——
    //   实测两边各超时、脚本拿到的全是空图。
    // ⚠ 这里要**顺带推一次合成**(refresh 自带 33ms 节流):
    //   · 只靠"窗口推帧 -> 投递 -> 主线程合成"那条链的话, 取图方在链路没跟上时会一直读到旧帧
    //     —— 实测虚拟进程里的脚本开局好几秒"只看到窗口刚 show 时的客户区"(背景色在、飞船和
    //     敌机都不在), 颜色匹配全落空;
    //   · 但也不能每次取图都无脑合成: 合成是 O(像素) 的重活, 一拍问几次颜色就把主线程压垮
    //     (实测一拍从 0.6s 涨到 6s)。refresh 里的 33ms 节流正好卡住这个量级: 最多 30 次/秒,
    //     既保证"取到的不超过 33ms 旧", 又不会积压。
        refresh();
        return desktopImage();               // 拷贝很轻 (像素缓冲共享)
}

JZImage JZVirtualDevice::screenshotWindow(JZVirtualWindow* w)
{
    if (!w)
        return JZImage();
    // ⚠ 优先用窗口**已经推上来的那一帧**(m_frame): 那是窗口线程画完自己拷好的, 读它
    //   **不用打扰窗口线程**。以前这里无条件 snapshot(true) —— 那会让窗口线程"额外渲染一帧"
    //   再跨线程拷一份(两次投递, 各最多等 2 秒), 与游戏自己的绘制 / 注入的按键挤在同一个
    //   线程的队列里: 外面每 400ms 抓一次就把队列压住, 连按键都要排队(实测: 虚拟进程里的
    //   脚本一拍卡 3.7 秒 —— 那几拍它什么都做不了, 只能挨打)。
    //   只有"这个窗口还从没画过"(缓存空, 比如刚建好/还没出首帧)时才退化到现场渲染一次。
    const JZImage& f = w->frame();
    if (!f.isNull() && f.data())
        return f;                        // 拷贝很轻 (像素缓冲共享)
    return w->snapshot(true);
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
    JZVirtualWindow* closeTarget = nullptr;      // 点了关闭按钮的窗口: 也锁外做 (见 destroyWindow)
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
                    // 点关闭 = **发一条 close 消息** (== PostMessage(hwnd, WM_CLOSE); 见
                    //   JZVirtualWindow::close): 由窗口**自己**处理 —— 派发 closeEvent, 应用可
                    //   ignore() 拒绝; 接受才 hide + 从桌面摘下来 (detachWindow, 客户区对象保留)。
                    //   ⚠ 只记下来, **锁外**发: 客户区可能在别的线程上, 那条路是投递,
                    //   握着 m_mutex 做事会跟"那个线程回头抢这把锁"撞上 (同 activate 那条注释)。
                    //   点关闭不激活 (消息发完就完事, 与以前一致)。
                    closeTarget = w;
                    activateTarget = nullptr;
                    break;
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

    // ---- 锁外这一步, 四件事的顺序与"真桌面 + 真系统"一致: 先是关窗 (它把这次点击吃掉了),
    //      再激活, 然后执行 chrome 动作, 最后把事件投给客户区。⚠ 四者**都**不能握着锁做
    //      (见 activate / destroyWindow / 下面的注释)。 ----
    if (closeTarget) {
        closeTarget->close();        // 发 close 消息 (跨线程 = 投递, 不等)
        return true;
    }

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

// 输入法 (组字/上屏/候选定位): 与键盘同一套投递纪律 (锁外投给活动窗口的客户区)。
// ⚠ 中文那条路只走这里 —— IME 的上屏内容**不**产生 key 事件 (真窗口同形状:
//   WM_IME_COMPOSITION 与 WM_CHAR 是两条消息)。漏了这一步就是"能打字母, 打不了中文"。
bool JZVirtualDevice::injectIme(const JZImeEvent& ev)
{
    JZVirtualWindow* w = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        w = m_active;
    }
    return w ? w->injectIme(ev) : false;
}

bool JZVirtualDevice::imeWanted() const
{
    std::lock_guard<std::recursive_mutex> lk(m_mutex);
    return m_active && m_active->imeWanted();
}

void JZVirtualDevice::notifyImeChanged()
{
    runOnView([](JZVirtualScreenView* v) { updateHostImeContextOf(v); });
}

// 显示控件活在**宿主窗口的线程**上, 而调用方常常是某个虚拟窗口自己的线程 (要不要输入法、
// 候选窗位置都由那边的焦点控件说了算) ⇒ 一律转投过去, 不直接跨线程摸宿主的控件。
// ⚠ 因此 fn 不能握有任何锁, 也不能回等调用方线程 (这条与 refresh() 同一套纪律)。
void JZVirtualDevice::runOnView(std::function<void(JZVirtualScreenView*)> fn)
{
    JZVirtualScreenView* v = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        v = m_view;
    }
    if (!v)
        return;                                   // 无头: 没有宿主可谈
    JZThread* owner = v->thread();
    if (owner && JZThread::currentThread() != owner) {
        // fn 要比这次调用活得久 (lambda 稍后才跑): 装进 shared_ptr 带着走。
        auto body = std::make_shared<std::function<void(JZVirtualScreenView*)>>(std::move(fn));
        JZObjectPointer guard = v->guardPointer();        // 控件没了这次自动作废
        runOnObject(guard, [guard, body]() {
            if (JZVirtualScreenView* vv = (JZVirtualScreenView*)guard.object())
                (*body)(vv);
        });
        return;
    }
    fn(v);
}

void JZVirtualDevice::updateHostImeContextOf(JZVirtualScreenView* v)
{
    if (JZWindow* host = v->window())
        host->updateImeContext();   // 重算: 内部读 JZVirtualScreenView::imeEnabled()
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
