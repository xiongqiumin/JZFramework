// ==================== 顶层窗口 (框架侧) ====================
// 本文件**不含任何平台调用**: 建窗/显隐/移动/抓取/光标/DPI/输入法/模态循环全部转发给
// JZWindowBackend (真窗口 = JZWin32WindowBackend; 虚拟桌面 = JZVirtualWindowBackend)。
//
// 这里留下的是"窗口的**行为**": 控件树/布局、命中测试、事件分发(鼠标/键盘/IME/焦点)、
// 悬停跟踪、输入蒙版、焦点与 IMC 语义、关闭流程、绘制调度 —— 两个平台共用同一份,
// 于是"虚拟窗口与真实窗口行为一致"是结构上保证的, 不是两边各写一遍去对齐。
#include "JZWindow.h"
#include "JZWindowBackend.h"
#include "JZWindowHost.h"
#include "JZDisplayDevice.h"
#include "JZPainter.h"
#include "JZPainterEvent.h"
#include "JZImeEvent.h"
#include "JZApplication.h"
#include "JZWin32Thread.h"   // 弹层唤醒用 (wakeUp 是 protected, 由主线程转发) + 键码定义

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

JZ_OBJECT_IMPL(JZWindow, JZWidget)

// 未显式设定尺寸的窗口 (构造不再预置 800×600): 首次显示时先按 sizeHint 自适应;
// sizeHint 也给不出 (无布局无子控件, 叶子 sizeHint = 当前尺寸 = 0×0) 再退回该默认,
// 避免开出 0×0 的不可见窗口。
static const int kDefaultWindowW = 800;
static const int kDefaultWindowH = 600;

// ---- 后端接口的默认实现 (与平台无关的那部分) ----
// 默认按**客户区**判定: 客户区左上角(clientToScreen(0,0)) + 窗口尺寸。
// 真窗口可重写成"窗口外框"的口径 (见 JZWin32WindowBackend::containsScreenPoint)。
bool JZWindowBackend::containsScreenPoint(JZWindow* w, int x, int y) const
{
    if (!w)
        return false;
    const JZPoint tl = w->clientToScreen(JZPoint(0, 0));
    return x >= tl.x() && x < tl.x() + w->width() &&
           y >= tl.y() && y < tl.y() + w->height();
}

JZWindow::JZWindow(JZWidget* parent)
    : JZWidget(parent)
{
    m_device = new JZDisplayDevice();
    m_backend = jzCreateDefaultWindowBackend();   // 当前平台的实现 (Win32 / 将来其它)
    m_visible = false;

    // 不预置尺寸: 留 0×0, 首次 showEvent 里按 sizeHint 自适应 (见 showEvent)。
    // 最小尺寸由布局 minimumSizeHint 自动推导 (见 JZWidget::minimumWidth/Height),
    // 窗口拖动缩放时不会小于内容所需。

    // 窗口宿主: "之后新建的窗口归谁托管" (见 JZWindowHost.h)。
    // 宿主在这里把本窗口的后端换成自己的实现 —— 例如虚拟桌面: 这台窗口从此活在
    // 那块桌面上 (JS 里 new JZWindow(...) 也一样, 因为它就走这条构造)。
    // ⚠ 必须早于 show() (那才会建平台窗口) 与下面的顶层窗口登记 (宿主可能把它标成
    //   "内部窗口": 虚拟桌面上的窗口不该混进宿主 app 的窗口生命周期判定)。
    JZWindowHost* host = jzWindowHost();
    if (host)
        host->onWindowCreated(this);

    // 登记到顶层窗口列表 (JZApplication::topLevelWindows / activeWindow,
    // 以及"最后一个窗口关闭则退出"的判定都基于它)
    if (jzApp && m_topLevelWindow)
        jzApp->addTopLevelWindow(this);
}

JZWindow::JZWindow(int w, int h, const std::string& title, JZWidget* parent)
    : JZWindow(parent)
{
    resize(w, h);
    setTitle(title);
}

JZWindow::~JZWindow()
{
if (jzApp)
    jzApp->removeTopLevelWindow(this);

    if (m_backend) {
        m_backend->destroy(this);     // 真窗口: DestroyWindow; 虚拟: 无原生资源
        delete m_backend;
        m_backend = nullptr;
    }

    delete m_device;
    m_device = nullptr;
}

// ==================== 平台后端 ====================
void JZWindow::setWindowBackend(JZWindowBackend* backend)
{
    if (m_backend == backend)
        return;
    if (m_backend) {
        m_backend->destroy(this);
        delete m_backend;
    }
    m_backend = backend;      // 接管所有权 (nullptr 时不换: 下一个平台实现由工厂给)
}

// 平台窗口句柄: 问后端要 —— 两个平台各有自己的"窗口对象", 没有"谁恒为 nullptr"的说法。
// 窗口未创建(惰性)或后端缺失时返回 nullptr (真窗口 = 还没有 HWND)。
void* JZWindow::nativeHandle() const
{
    return m_backend ? m_backend->handle() : nullptr;
}

JZPoint JZWindow::clientToScreen(const JZPoint& pt) const
{
    return m_backend ? m_backend->clientToScreen(const_cast<JZWindow*>(this), pt) : pt;
}

JZPoint JZWindow::screenToClient(const JZPoint& pt) const
{
    return m_backend ? m_backend->screenToClient(const_cast<JZWindow*>(this), pt) : pt;
}

JZImage JZWindow::grabClient()
{
    return m_backend ? m_backend->grabClient(this) : JZImage();
}

void JZWindow::setTitle(const std::string& title)
{
    m_title = title;
    if (m_backend)
        m_backend->setTitle(this, title);
}

std::string JZWindow::title() const
{
    return m_title;
}

JZDisplayDevice* JZWindow::device()
{
    return m_device;
}

void JZWindow::setDeviceScale(float scale)
{
    m_scaleExplicit = true;
    m_device->setScale(scale);
}

// 重绘指定区域: 遍历控件树(含剔除) -> 各控件 paintEvent -> backend 渲染
void JZWindow::doPaint(const JZRect& rc)
{
    if (!m_device || !m_windowCreated)
        return;
    if (m_painting)         // 绘制中再被直接调进来 (resize/show 那几条同步路径): 不重入
        return;
    m_painting = true;
    m_device->render(this, rc);
    m_painting = false;
    if (m_backend)
        m_backend->onPainted(this);     // 虚拟窗口: 把新帧推给桌面 (真窗口: 无事可做)
}

// 惰性创建 (首次 showEvent 触发): 具体做什么由后端决定 ——
// 真窗口 = 注册窗口类 + CreateWindowExW + device->init(hwnd,...);
// 虚拟窗口 = 建离屏帧缓冲 (缩放 = 它所在桌面的 DPI, 默认 1)。成功才把窗口标成"已创建"。
void JZWindow::createWindow()
{
    if (m_windowCreated || !m_backend)
        return;
    if (!m_backend->create(this))
        return;
    m_windowCreated = true;

    // 登记成系统拖放目标 (外部拖文件/图片进来)。窗口已创建, 句柄有效;
    // OLE 平台代码在 JZWin32WindowBackend::registerDragAndDrop 里。
    if (m_backend)
        m_backend->registerDragAndDrop(this);
}

void JZWindow::showEvent(JZShowEvent* event)
{
    JZWidget::showEvent(event);

    // 首次显示且未显式设定尺寸 (0×0): 按 sizeHint 自适应, 再不行退默认尺寸。
    // 必须发生在 createWindow 之前, 原生窗口一次就按最终尺寸创建, 无跳变。
    // 之后 size 已非 0, 重复 show 不会重算; close() 后再 show 沿用上次尺寸 (Qt 语义)。
    if (width() <= 0 && height() <= 0)
    {
        JZSize hint = sizeHint();
        if (hint.width() <= 0 || hint.height() <= 0)
            hint = JZSize(kDefaultWindowW, kDefaultWindowH);
        resize(hint.width(), hint.height());
    }

    createWindow();
    if (m_windowCreated && m_backend)
        m_backend->show(this);
}

void JZWindow::hideEvent(JZHideEvent* event)
{
    JZWidget::hideEvent(event);

    if (m_windowCreated) {
        // 窗口隐藏: 任何进行中的鼠标抓取都失效 (控件可能已被移除/不可见)
        if (m_mouseGrabber)
            releaseMouse(nullptr);
        // 悬停态随窗口一起失效, 下次显示后由 mouseMove 重新建立
        m_hoverWidget = nullptr;
        if (m_backend)
            m_backend->hide(this);
    }

    // 可见性变化同步到 jzApp 顶层窗口状态: notifyWindowClosed 以
    // "isVisible() && nativeHandle()" 判定是否还有活着的顶层窗口。
    // 只在销毁时通知会漏掉"先隐藏后销毁/只隐藏"的路径 —— 典型如文件→退出时
    // 菜单弹层还开着, 主窗口销毁后弹层才隐藏, 便再没人通知, 进程残留。
    if (jzApp)
        jzApp->notifyWindowClosed(this);
}

// ==================== 鼠标抓取 ====================
void JZWindow::grabMouse(JZWidget* w)
{
    if (!w)
        return;
    m_mouseGrabber = w;          // 后续鼠标事件直发 w (见 dispatchMouse)
    if (m_backend)
        m_backend->setCapture(this, true);   // 真窗口: SetCapture (含客户区外的 move/up)
}

void JZWindow::releaseMouse(JZWidget* w)
{
    // w==nullptr: 强制解除; 否则仅当 w 是当前 grabber 时才解除 (避免误伤别人的抓取)
    if (w && w != m_mouseGrabber)
        return;
    m_mouseGrabber = nullptr;
    if (m_backend)
        m_backend->setCapture(this, false);
}

// ==================== 弹层外部按下 (替代 SetCapture) ====================
bool JZWindow::filterPopupPress(int screenX, int screenY)
{
    if (!jzApp)
        return false;

    // 收集当前所有可见的 Popup 弹层 (顶层窗口表里就有, 无需自建登记表)
    std::vector<JZWindow*> popups;
    for (JZWindow* w : jzApp->topLevelWindows())
    {
        if (w->windowType() == Popup && w->isVisible() && w->nativeHandle())
            popups.push_back(w);
    }
    if (popups.empty())
        return false;

    bool insideAny = false;
    for (JZWindow* w : popups)
    {
        // "这个点在不在该弹层里"由**后端**回答 (真窗口 = 窗口外框 GetWindowRect):
        // 平台无关的循环留在框架, 平台口径留在后端。
        if (w->windowBackend() && w->windowBackend()->containsScreenPoint(w, screenX, screenY))
        {
            insideAny = true;
            break;
        }
    }

    // 关闭所有不含按下点的弹层: 子菜单开着时点父菜单 -> 只关子菜单, 父菜单照常响应
    bool closed = false;
    for (JZWindow* w : popups)
    {
        if (!(w->windowBackend() && w->windowBackend()->containsScreenPoint(w, screenX, screenY)))
        {
            w->hide();
            closed = true;
        }
    }
    // 弹层隐藏即退出 execModal, 但它正阻塞在 GetMessage 上; 立刻唤醒避免迟滞
    if (closed)
    {
        if (auto* mt = static_cast<JZWin32Thread*>(jzApp->mainThread()))
            mt->wakeUp();
    }

    // 按下点在所有弹层之外: 吞掉这次按下 (Qt 菜单语义: 外部点击只用于关闭)
    return !insideAny;
}

// ==================== 输入屏蔽 (全屏蒙版) ====================
void JZWindow::setInputGate(JZWidget* gate)
{
    m_inputGate = gate;
    // 屏蔽上闸期间正在组字的输入法应整体取消, 否则组字窗口/上屏状态悬空
    if (gate && m_focusWidget && m_focusWidget->imeEnabled())
        cancelImeComposition();
}

// hit 是否等于 gate 自身或位于其子树内 (全屏蒙版只放行自身子树, 如"取消"按钮)
static bool jzWindowHitInGate(JZWidget* hit, JZWidget* gate)
{
    for (JZWidget* w = hit; w; w = w->parentWidget()) {
        if (w == gate)
            return true;
    }
    return false;
}

// 按当前光标位置 SetCursor: 取目标控件的生效光标 JZWidget::cursor()
// (自身 setCursor 或沿父链继承, 全无则箭头)。实际动作在后端 (它有系统光标)。
void JZWindow::applyMouseCursor()
{
    if (m_backend)
        m_backend->updateCursor(this);
}

void JZWindow::paintEvent(JZPainterEvent* event)
{
    // 窗口底色 (内容控件可覆盖)
    JZPainter p(this);
    p.fillRect(rect(), style().background.valueOr(JZColor::White));
}

JZStyleSet JZWindow::uaStyle() const
{
    JZStyleSet s;
    s.background = JZOptional<JZColor>::Some(JZColor::White);
    return s;
}

void JZWindow::resize(int w, int h)
{
    bool changed = (w != width() || h != height());
    JZWidget::resize(w, h);
    if (changed && m_backend)
        m_backend->setClientSize(this, w, h);   // 真窗口: SetWindowPos -> WM_SIZE; 虚拟: 重建离屏
}

// 原生窗口尺寸变化 (WM_SIZE): 更新根控件几何 + 立即整窗重绘。
// resizeEvent 已由 JZWidget::resize 统一派发 (窗口与普通控件一视同仁), 此处不再手动补发。
// 关键: 拖动 resize 时 WM_SIZE 由系统模态循环派发, 该循环不经过框架事件队列
// (processAndPaint/processEvents 跑不到) —— 只 update() 排队的话拖动全程画面不刷新,
// 松手才补一帧; 且位图已按新客户区重建, 未绘制像素直接 BitBlt 上屏会露未初始化
// DIB 内存。因此这里直接同步渲染, 拖动过程每步实时重绘。
void JZWindow::windowResized(int logicalW, int logicalH)
{
    if (logicalW != width() || logicalH != height())
        JZWidget::resize(logicalW, logicalH);

    doPaint(rect());
}

// 显示器 DPI 变化 (WM_DPICHANGED): 更新设备缩放 (重建字体)。新 scale 由后端给
// (真窗口 = 该窗口所在显示器的 DPI; 虚拟窗口 = 它所在那块桌面的缩放)。
void JZWindow::dpiChanged()
{
    if (m_device && m_backend)
        m_device->setScale(m_backend->deviceScale(this));
}

// ==================== 焦点管理 ====================
void JZWindow::setFocusWidget(JZWidget* w)
{
    if (m_focusWidget == w)
        return;
    JZWidget* old = m_focusWidget;
    m_focusWidget = w;
    if (old) {
        JZEvent ev(JZEvent_focusOut);       // 先派发: 控件在此取消未上屏的组字
        jzApp->sendEvent(old, &ev);
    }
    // 焦点切换后重算 IMC: 焦点落到非文本控件上就断开,
    // 否则打字会在后台组字却没有任何控件显示它。
    updateImeContext();
    if (w) {
        JZEvent ev(JZEvent_focusIn);
        jzApp->sendEvent(w, &ev);
    }
}

// ==================== 命中测试 ====================
// 从根向下, 返回包含全局逻辑坐标的最深层可见控件 (不含子窗口 / 根自身)。
JZWidget* JZWindow::hitTest(const JZPoint& global)
{
    return hitTestRec(this, global);
}

JZWidget* JZWindow::hitTestRec(JZWidget* w, const JZPoint& global)
{
    const auto& ch = w->children();
    for (int i = (int)ch.size() - 1; i >= 0; --i) {
        JZWidget* c = jzobject_cast<JZWidget*>(ch[i]);
        if (!c || jzobject_cast<JZWindow*>(c))   // 跳过子窗口 / 非控件
            continue;
        if (!c->isVisible())
            continue;
        JZRect gr = c->mapToGlobal(c->rect());
        if (global.x() >= gr.left() && global.x() < gr.right() &&
            global.y() >= gr.top()  && global.y() < gr.bottom()) {
            if (c->isPassThrough()) {
                // 透传覆盖层 (AI 鼠标等): 自身不拦截, 只考虑其子控件 (如取消按钮)
                JZWidget* deeper = hitTestRec(c, global);
                if (deeper)
                    return deeper;
                continue;   // 透传给更下层的控件
            }
            JZWidget* deeper = hitTestRec(c, global);
            return deeper ? deeper : c;
        }
    }
    return nullptr;
}

// 收集 from 到 root(不含) 的祖先链 (from 在链头, root 之前结束)。
static void collectHoverChain(JZWidget* from, JZWidget* root, std::vector<JZWidget*>& chain)
{
    for (JZWidget* w = from; w && w != root; w = w->parentWidget())
        chain.push_back(w);
}

void JZWindow::updateHover(JZWidget* newHover)
{
    if (newHover == m_hoverWidget)
        return;

    std::vector<JZWidget*> oldChain, newChain;
    collectHoverChain(m_hoverWidget, this, oldChain);
    collectHoverChain(newHover, this, newChain);

    // 找最近公共祖先: 从链尾 (最接近 root) 向上比对, 公共前缀不触发事件
    int i = (int)oldChain.size() - 1;
    int j = (int)newChain.size() - 1;
    while (i >= 0 && j >= 0 && oldChain[i] == newChain[j]) {
        --i; --j;
    }

    // leave: 旧链中不再悬停的控件, 由深到浅 (oldChain[i] .. oldChain[0])
    JZLeaveEvent leaveEv;
    for (int k = i; k >= 0; --k)
        jzApp->sendEvent(oldChain[k], &leaveEv);

    // enter: 新链中开始悬停的控件, 由浅到深 (newChain[0] .. newChain[j])
    JZEnterEvent enterEv;
    for (int k = 0; k <= j; ++k)
        jzApp->sendEvent(newChain[k], &enterEv);

    m_hoverWidget = newHover;
}

// ==================== 事件统一入口 ====================

// 事件队列派发入口: invalidate 在此转给**平台后端**排进消息循环 (见 onInvalidate)。
// 不再由线程扫队列抽取 —— 那依赖事件在快照前后的时序, 定时器回调里 update()
// 发出的重绘会被派发循环吞掉 (画面冻结); 走正常事件派发则任何上下文都不丢。
bool JZWindow::event(JZEvent* e)
{
    if (e->type() == JZEvent_invalidate)
    {
        JZInvalidateEvent* ev = (JZInvalidateEvent*)e;
        // 脏区交给平台后端, 由它排进**某个消息循环** (JZWindow 不关心谁画、在哪画):
        //   真窗口   = 主线程的 paint task (WM_PAINT 同款);
        //   虚拟窗口 = 窗口所属虚拟进程的线程 (那个进程有自己的事件循环)。
        // ⚠ 本事件是在窗口所属线程上派发的 (同线程直投 / 跨线程投递), 而画总是排给那个
        //   线程的循环 ⇒ 帧缓冲的主人只有一个线程, 不会出现"替别的线程画窗口"。
        if (m_backend)
            m_backend->onInvalidate(this, ev->rect);
        else
            doPaint(ev->rect);       // 没有后端(退化): 没循环可排, 就地画一帧
        return true;
    }

    // 关闭事件 (Qt: QWidget::event(QEvent::Close) -> closeEvent()):
    // 经 sendEvent / postEvent 到达时路由给 closeEvent, 返回值即"是否接受关闭"。
    if (e->type() == JZEvent_close)
    {
        closeEvent((JZCloseEvent*)e);
        return e->isAccepted();
    }


    // 队列派发的合成鼠标 (postEvent, 如 AI 驱动的点击): 走与 WndProc 实时
    // 路径同构的命中测试分发。点击引发的槽函数在此派发栈内执行 —— 其中的
    // 模态循环不阻塞调用方 (如 AI 驱动已先收尾回报)。
    if (e->type() == JZEvent_mouse)
        return onWindowProcess(e);

    // 键盘/IME 同理: 它们的目标是**焦点控件** (见 dispatchKey/dispatchIme), 不是窗口自己 ——
    // 不在这里转交的话会落到 JZWidget::event -> 窗口自己的 keyPressEvent 上, 焦点控件永远
    // 收不到 (表现为"注入了按键但输入框还是空的")。
    // ⚠ 真窗口那条路 (WndProc) 本来就是直接调 onWindowProcess, 所以以前没人走到这里;
    //   走这条路的正是**合成事件**: 虚拟桌面 (JZVirtualWindow::injectKey) 与队列派发的按键。
    // ⚠ 闸: dispatchKey 的焦点链最后一级就是**窗口自己** (焦点控件沿 parentWidget 一路向上),
    //   那一级会 sendEvent(this, ...) 再回到这里 ⇒ 不挡就是无限递归 (实测栈溢出)。
    //   挡住之后走 JZWidget::event —— 正是那一级本来该做的事, 语义不变。
    if (e->type() == JZEvent_key || e->type() == JZEvent_ime)
    {
        if (m_dispatchingKey)
            return JZWidget::event(e);
        m_dispatchingKey = true;
        const bool handled = onWindowProcess(e);
        m_dispatchingKey = false;
        return handled;
    }

    return JZWidget::event(e);
}

// 平台层把消息翻译成 JZEvent 后调到这里, 按类型分发:
//   鼠标 -> 命中测试定位目标控件;  键盘 -> 从焦点控件逐级向上传播;
//   IME  -> 焦点控件;              窗口焦点 -> onWindowFocus (IMC + 焦点事件)。
// 返回 true 表示事件已被处理 (真窗口的 WndProc 不再转交 DefWindowProc)。
bool JZWindow::onWindowProcess(JZEvent* event)
{
    switch (event->type()) {
    case JZEvent_mouse:
        return dispatchMouse((JZMouseEvent*)event);
    case JZEvent_key:
        return dispatchKey((JZKeyEvent*)event);
    case JZEvent_ime:
        // 无焦点控件时返回 false, 交回平台层 —— 不要静默吞掉,
        // 否则 IME 消息没有去处, 组字内容会凭空消失。
        return dispatchIme((JZImeEvent*)event);
    case JZEvent_focusIn:
        onWindowFocus(true);
        return true;
    case JZEvent_focusOut:
        onWindowFocus(false);
        return true;
    default:
        return false;
    }
}

// ==================== 输入分发 ====================
// 鼠标: 命中测试定位最深可见控件, 回填 localPos 后投递; 左键按下同时切换焦点。
bool JZWindow::dispatchMouse(JZMouseEvent* ev)
{
    // ---- 全屏蒙版输入屏蔽 (AI 操作期间) ----
    // 丢弃真实用户的鼠标 (按/放/移/滚轮), 只放行蒙版自身子控件 (取消按钮);
    // AI 合成的程序化点击 (isProgrammatic) 不受限制, 可穿透蒙版落到目标控件;
    // 鼠标抓取 (拖拽) 进行中不屏蔽: 抓取中的 move/up 必须完整送达, 否则拖死。
    if (m_inputGate && m_inputGate->isVisible() && !ev->isProgrammatic() && !m_mouseGrabber) {
        JZWidget* hit = hitTest(ev->globalPos());
        if (!hit || !jzWindowHitInGate(hit, m_inputGate))
            return true;   // 事件丢弃, 不外泄到蒙版下的控件
    }

    // 悬停跟踪: mouseMove 命中控件变化时派发 enter/leave (拖拽抓取 / AI 蒙版期间不跟踪,
    // 避免弹层/提示在错误时机弹出)。press/release/wheel 不改悬停态。
    if (!m_mouseGrabber && !(m_inputGate && m_inputGate->isVisible()) &&
        ev->mouseType() == JZMouseEvent::Move) {
        updateHover(hitTest(ev->globalPos()));
    }

    // 滚轮冒泡 (Qt 语义): 从最深的可见控件沿父链逐级投递, 未消费的
    // (handler 未调用 accept) 继续向上, 让外层滚动容器 (JZScrollArea) 处理
    // 内容控件/嵌套子控件上方的滚轮。鼠标抓取期间不冒泡 (拖拽目标独占)。
    if (!m_mouseGrabber && ev->mouseType() == JZMouseEvent::Wheel) {
        for (JZWidget* w = hitTest(ev->globalPos()); w; w = w->parentWidget()) {
            // 顶层内容控件的 parent 就是 JZWindow 自身: 若继续冒泡回 window,
            // JZWindow::event() 会把 mouse 当新消息再进 dispatchMouse, 从 hitTest
            // 重新分发同一滚轮 -> window <-> 控件无限递归 (栈溢出)。
            // Qt 语义: 滚轮冒泡止于内容控件, 不进入 QWindow 做二次分发。
            if (jzobject_cast<JZWindow*>(w))
                break;
            ev->ignore();   // 每一级重新给处理机会
            JZPoint gTop = w->mapToGlobal(w->rect().topLeft());
            ev->setLocalPos(JZPoint(ev->globalPos().x() - gTop.x(),
                                    ev->globalPos().y() - gTop.y()));
            jzApp->sendEvent(w, ev);
            if (ev->isAccepted())
                return true;
        }
        return true;   // 无人消费也吞掉, 不外泄到平台层
    }

    // 鼠标抓取: 所有鼠标事件 (含窗口客户区外) 直发 grabber, 跳过命中测试。
    // 用于拖拽, 光标冲出控件命中带也不丢 move/up。
    JZWidget* hit = m_mouseGrabber ? m_mouseGrabber : hitTest(ev->globalPos());
    if (hit) {
        // localPos = 屏幕坐标 - 命中控件屏幕左上角 (Qt 语义: globalPos - mapToGlobal(rect.topLeft))
        JZPoint gTop = hit->mapToGlobal(hit->rect().topLeft());
        ev->setLocalPos(JZPoint(ev->globalPos().x() - gTop.x(),
                                ev->globalPos().y() - gTop.y()));
        jzApp->sendEvent(hit, ev);   // 同步派发, 过 hit 的事件过滤器链
        // 左键按下: 把焦点交给命中控件 (点击切换焦点)
        if (ev->mouseType() == JZMouseEvent::Press && ev->button() == JZMouseEvent::LeftButton)
            hit->setFocus();
    } else if (ev->mouseType() == JZMouseEvent::Press && ev->button() == JZMouseEvent::LeftButton) {
        setFocusWidget(nullptr);   // 点空白: 清空焦点
    }
    // 派发后即时按事件改动的状态 (hover/拖动) 刷新光标:
    // 抓取期间系统不发 WM_SETCURSOR, 只靠它会在拖动全程不更新。
    applyMouseCursor();
    return true;
}

// 键盘: 从焦点控件开始, 未被 accept 的键逐级向上 (父控件) 传播,
// 直到被接受或整条控件链都不要 -> 返回 false, 由平台层转交默认处理
// (Tab / Alt 菜单等系统默认行为)。
bool JZWindow::dispatchKey(JZKeyEvent* ev)
{
    // ---- 全屏蒙版输入屏蔽 (AI 操作期间) ----
    // 键盘整体丢弃 (焦点控件收不到), 仅 Esc = 取消: 交给蒙版自己处理。
    if (m_inputGate && m_inputGate->isVisible()) {
        if (ev->isPress() && ev->key() == VK_ESCAPE) {
            ev->ignore();
            jzApp->sendEvent(m_inputGate, ev);
            return ev->isAccepted();
        }
        return true;   // 其余按键全部丢弃
    }

    for (JZWidget* w = m_focusWidget; w; w = w->parentWidget()) {
        ev->ignore();   // 每一级重新给处理机会
        jzApp->sendEvent(w, ev);   // 同步派发; 每级先过该控件的事件过滤器链
        if (ev->isAccepted())
            return true;
    }
    return false;
}

// IME: 组字/上屏/候选事件只投递给焦点控件; 无焦点控件返回 false (交回系统)。
bool JZWindow::dispatchIme(JZImeEvent* ev)
{
    // ---- 全屏蒙版输入屏蔽 (AI 操作期间) ----
    if (m_inputGate && m_inputGate->isVisible())
        return true;   // 丢弃 IME 组字/上屏 (蒙版期间焦点控件不允许进字)

    if (!m_focusWidget)
        return false;
    jzApp->sendEvent(m_focusWidget, ev);   // 同步派发, 过焦点控件的过滤器链
    return true;
}

// ==================== 拖放 (控件树分发; OLE 平台代码在 JZWin32WindowBackend) ====================
//
// dragEnter/dragOver/dragLeave/dragDrop 是平台侧 OLE 回调翻译后调进来的入口
// (见 JZWin32WindowBackend::registerDragAndDrop 的 JZWindowDropTarget); 本段只做
// "命中控件 + 沿父链上抛"的框架分发 (语义与 Qt 一致): 未开 acceptDrops 的控件透明,
// 整条链都没人接时拖方显示"禁止"。

// 注销 OLE drop target: 委托给平台后端 (HWND 在后端里)。必须在窗口句柄销毁前调用,
// 否则 RevokeDragDrop 找不到句柄。
void JZWindow::revokeDropTarget()
{
    if (m_backend)
        m_backend->revokeDragAndDrop(this);
    dragResetTarget(false);
}

JZWidget* JZWindow::dragTarget() const
{
    JZObject* obj = m_dragTarget.object();
    return obj ? jzobject_cast<JZWidget*>(obj) : nullptr;
}

// 接收方选中的动作: 只 accept() 而没挑动作的按系统建议算 (Qt 中 accept() 亦含此意)。
static JZDropAction jzResolvedDropAction(const JZDropEvent* ev)
{
    JZDropAction action = ev->dropAction();
    if (action == JZDropActionNone || action == JZDropActionIgnore)
        action = ev->proposedAction();
    return action;
}

void JZWindow::dragChain(const JZPoint& globalLogical, std::vector<JZWidget*>& chain)
{
    chain.clear();
    // 最深的命中控件起逐级上溯, 最后补上窗口自身 —— 窗口本人开了 acceptDrops 也能当落点。
    for (JZWidget* w = hitTest(globalLogical); w; w = w->parentWidget())
        chain.push_back(w);
    chain.push_back(this);
}

bool JZWindow::dragDispatch(const std::vector<JZWidget*>& chain, JZDropEvent* ev)
{
    // 构造时 pos 里装的是屏幕逻辑坐标; 这里按每级目标换算成它的局部坐标。
    const JZPoint global = ev->pos();
    for (JZWidget* w : chain) {
        if (!w || !w->acceptDrops())
            continue;

        JZPoint gTop = w->mapToGlobal(w->rect().topLeft());
        ev->setPos(JZPoint(global.x() - gTop.x(), global.y() - gTop.y()));
        ev->ignore();               // 每一级重新给一次表态机会
        jzApp->sendEvent(w, ev);
        if (!ev->isAccepted())
            continue;

        m_dragTarget = w->guardPointer();
        m_dragAction = jzResolvedDropAction(ev);
        return true;
    }
    return false;
}

void JZWindow::dragResetTarget(bool notify)
{
    if (JZWidget* target = dragTarget()) {
        if (notify) {
            JZDragLeaveEvent ev;
            jzApp->sendEvent(target, &ev);
        }
    }
    m_dragTarget = JZObjectPointer();
    m_dragMime.reset();
    m_dragActions = JZDropActionNone;
    m_dragAction = JZDropActionNone;
}

bool JZWindow::dragEnter(const std::shared_ptr<JZMimeData>& mime, JZDropActions actions,
                         JZDropAction proposed, uint32_t modifiers, const JZPoint& globalLogical)
{
    dragResetTarget();              // 上一轮没清干净 (数据源异常退出) 时的兜底
    m_dragActions = actions;
    m_dragMime = mime;
    m_dragAction = JZDropActionNone;

    std::vector<JZWidget*> chain;
    dragChain(globalLogical, chain);

    JZDragEnterEvent ev(globalLogical, mime, actions, proposed, modifiers);
    return dragDispatch(chain, &ev);
}

bool JZWindow::dragOver(JZDropActions actions, JZDropAction proposed, uint32_t modifiers,
                        const JZPoint& globalLogical)
{
    if (!m_dragMime)
        return false;
    m_dragActions = actions;

    std::vector<JZWidget*> chain;
    dragChain(globalLogical, chain);

    // 还在原目标上 -> 直接发 move。目标是 dragEnter 那一刻定下的, 拖着经过它的子控件
    // 不会被抢走 (Qt 同款: target 一路上不变, 位置变了也还是它)。
    JZWidget* target = dragTarget();
    if (target && target->acceptDrops() && target->isVisible() &&
        std::find(chain.begin(), chain.end(), target) != chain.end()) {
        JZPoint gTop = target->mapToGlobal(target->rect().topLeft());
        JZDragMoveEvent ev(JZPoint(globalLogical.x() - gTop.x(), globalLogical.y() - gTop.y()),
                           m_dragMime, actions, proposed, modifiers);
        jzApp->sendEvent(target, &ev);
        if (ev.isAccepted()) {
            m_dragAction = jzResolvedDropAction(&ev);
            return true;
        }
        dragResetTarget();          // 目标中途反悔 -> 拖离, 按当前位置重新挑
    } else if (target) {
        dragResetTarget();          // 拖出了目标的范围 -> leave 旧的, 重新挑
    }

    JZDragEnterEvent ev(globalLogical, m_dragMime, actions, proposed, modifiers);
    return dragDispatch(chain, &ev);
}

void JZWindow::dragLeave()
{
    dragResetTarget();
}

bool JZWindow::dragDrop(JZDropActions actions, JZDropAction proposed, uint32_t modifiers,
                       const JZPoint& globalLogical)
{
    JZWidget* target = dragTarget();
    if (!target || !m_dragMime)
        return false;

    JZPoint gTop = target->mapToGlobal(target->rect().topLeft());
    JZDropEvent ev(JZEvent_drop, JZPoint(globalLogical.x() - gTop.x(), globalLogical.y() - gTop.y()),
                   m_dragMime, actions, proposed, modifiers);
    jzApp->sendEvent(target, &ev);
    const bool accepted = ev.isAccepted();

    dragResetTarget(false);         // drop 之后没有"拖离"一说, 直接收尾
    m_dragAction = accepted ? jzResolvedDropAction(&ev) : JZDropActionNone;
    return accepted;
}

// 前置并激活 (Qt 同名 activateWindow()): 未显示过则先 show (惰性建窗口),
// 然后交给**后端**做前置 + 争焦点 —— 真窗口 = SetForegroundWindow 等;
// 虚拟窗口 = 桌面管理器 raise + 焦点。焦点消息回流后 isActiveWindow() 即为真。
// 注意: 真窗口下系统允许"前台窗口切换"有条件限制, 可能只闪烁任务栏而不真的抢到焦点
// (这是 Win32 规则, 不是本函数能决定的) —— 故调用后仍应看 isActiveWindow()。
void JZWindow::activateWindow()
{
    if (!isVisible())
        show();            // 未创建的窗口不可能成为前台窗口
    if (m_backend)
        m_backend->activate(this);

    // 立刻刷新应用级"最近激活窗口"(也是 activeWindow() 在整体失焦时的回退值);
    // 真正的活跃状态仍由焦点消息回流后写入 m_hasWindowFocus。
    // 内部窗口(虚拟桌面上的窗口)不参与: 它的活动态在自己的管理器里。
    if (jzApp && m_topLevelWindow)
        jzApp->setActiveWindow(this);
}

// 平台侧焦点入口 (真窗口 = WndProc 的 WM_SETFOCUS/WM_KILLFOCUS; 无平台消息的窗口
// 由它的管理器调) —— 与 onWindowFocus 是同一条路, 只是给外部一个公开入口。
void JZWindow::setWindowFocus(bool got)
{
    onWindowFocus(got);
}

// ==================== 是否应用顶层窗口 ====================
void JZWindow::setTopLevelWindow(bool on)
{
    if (m_topLevelWindow == on)
        return;
    m_topLevelWindow = on;
    if (!jzApp)
        return;
    if (on)
        jzApp->addTopLevelWindow(this);       // 幂等
    else
        jzApp->removeTopLevelWindow(this);    // 顺便清掉 app 的"最近激活窗口"记录
}

// 焦点进/出: 派发焦点进/出事件, 并重算 IMC 关联。
// 同一个窗口内多控件共享一个 IMC, 关联/断开一律走 updateImeContext(),
// 控件自身不再各自操作 (否则一个控件断开会影响同窗口其它控件)。
void JZWindow::onWindowFocus(bool got)
{
    m_hasWindowFocus = got;
    if (got && jzApp && m_topLevelWindow)
        jzApp->setActiveWindow(this);       // JZApplication::activeWindow() 的回退值

    if (!got && m_focusWidget) {
        JZEvent ev(JZEvent_focusOut);       // 先派发: 控件在此取消未上屏的组字
        jzApp->sendEvent(m_focusWidget, &ev);
    }
    updateImeContext();
    if (got && m_focusWidget) {
        JZEvent ev(JZEvent_focusIn);
        jzApp->sendEvent(m_focusWidget, &ev);
    }
}

// IMC 关联 + "焦点是否接受输入法"的重算。前者是平台的事 (IMM), 交给后端;
// 后者是框架语义 (有窗口焦点 && 焦点控件 imeEnabled), 留在这一层。
//
// ⚠ 后端只保证"IMC 始终关联", 绝不按焦点频繁断开: 断开会让输入法把该窗口的
// 中/英状态重置成默认中文 ("一聚焦输入框输入法就自动变中文")。
void JZWindow::updateImeContext()
{
    if (m_backend)
        m_backend->imeAssociate(this);

    const bool want = m_hasWindowFocus && m_focusWidget && m_focusWidget->imeEnabled();
    const bool wasActive = m_imeActive;
    m_imeActive = want;

    // 焦点是否接受输入法发生了变化: 主动重发 WM_IME_SETCONTEXT 让 IME 更新 UI
    // (系统只在窗口得失焦点时发一次, 那时焦点控件可能还没定, 见 updateImeUi 说明)
    if (wasActive != want)
        updateImeUi(want);

    // 焦点不接受输入法: 取消未上屏的组字。CPS_CANCEL 会带回 WM_IME_ENDCOMPOSITION,
    // 由消息层派发给焦点控件清掉自绘的组字串。
    if (wasActive && !want)
        cancelImeComposition();
}

// 主动重发平台输入法的 UI 通知 (真窗口 = WM_IME_SETCONTEXT)。
// 组字窗由控件自绘, 故永远不带 ISC_SHOWUICOMPOSITIONWINDOW。
void JZWindow::updateImeUi(bool show)
{
    if (m_backend)
        m_backend->imeUpdateUi(this, show);
}

// 放弃本次组字 (不上屏)
void JZWindow::cancelImeComposition()
{
    if (m_backend)
        m_backend->imeCancel(this);
}

// 让输入法的组字窗/候选窗字体与本窗口一致, 并保证输入法处于打开状态。
// 平台细节在后端; "是否强制中文模式"是框架设置 (setImeForceNativeMode)。
void JZWindow::prepareIme(int fontPixelSize)
{
    if (m_backend)
        m_backend->imePrepare(this, fontPixelSize);
}

// ==================== 关闭流程 ====================
// Qt 语义的关闭: close() 同步派发 closeEvent (QApplication::sendEvent 习惯),
// 重写方以 accept/ignore 表态; 接受才真正关闭 (隐藏 + 销毁平台窗口)。
// WM_CLOSE -> close(), 业务代码也直接调 close() —— 没有单独的 requestClose。
// (JZDialog::closeEvent 会 ignore() 并转为 reject: 只隐藏、不销毁, 可复用。)
bool JZWindow::close()
{
    JZCloseEvent ev;
    jzApp->sendEvent(this, &ev);   // -> event(JZEvent_close) -> closeEvent(&ev)
    if (!ev.isAccepted())
        return false;                  // 重写方 ignore(): 拒绝关闭 (如对话框转 reject)

    // 接受关闭: 先 hide() 同步可见性, 再销毁平台窗口。两条路径都会走到
    // jzApp->notifyWindowClosed, 由它判定"最后一个可见顶层窗口已关"并退出
    // 事件循环。(只靠"平台窗口销毁"通知一次会漏: 主窗口在菜单弹层还开着时被
    // 关闭 —— 如文件→退出 —— 弹层随后才 hide, 便再没人通知, 进程残留。)
    hide();
    if (m_backend)
        m_backend->destroy(this);      // 真窗口: DestroyWindow -> WM_DESTROY -> nativeDestroyed
    return true;
}

// 默认实现为空: JZCloseEvent 构造即 accepted, close() 据此执行关闭。
// 需要拦截关闭的窗口 (如 JZDialog) 重写后 ignore() 并自行处理。
void JZWindow::closeEvent(JZCloseEvent*)
{
}

// 平台窗口已销毁 (WM_DESTROY / 桌面收走): 清宿主状态避免句柄悬空;
// 之后 show() 经 createWindow() 会重新创建。
void JZWindow::nativeDestroyed()
{
    revokeDropTarget();     // 拖拽过程中窗口被销毁的路径: 先把 drop target 注销 (句柄还有效)
    if (m_backend)
        m_backend->onPlatformDestroyed(this);
    m_windowCreated = false;
    m_focusWidget = nullptr;
    m_hasWindowFocus = false;
    m_imeGotResult = false;
    m_imeActive = false;

    // 最后一个可见顶层窗口关闭 -> 退出事件循环 (jzApp->quit())。
    // 可用 jzApp->setQuitOnLastWindowClosed(false) 关掉这个行为。
    if (jzApp)
        jzApp->notifyWindowClosed(this);
}

// ==================== 窗口属性 ====================

// 置顶: 已创建的平台窗口立即生效, 未创建的记录标记待创建时套用后端的创建流程
void JZWindow::setTop(bool on)
{
    if (m_top == on)
        return;
    m_top = on;
    if (m_backend)
        m_backend->setTop(this, on);
}

void JZWindow::showMinimized()
{
    if (m_backend)
        m_backend->showMinimized(this);
}

void JZWindow::showMaximized()
{
    if (m_backend)
        m_backend->showMaximized(this);
}

void JZWindow::showNormal()
{
    if (m_backend)
        m_backend->showNormal(this);
}

// 屏幕逻辑坐标定位 (popup 等自定位窗口): 物理像素换算在后端。
// 惰性: 窗口未创建时只记录 (后端 create 时作为初始位置), 显式定位会关闭
// 显示自动居中 (等价 Qt 的 WA_Moved)。
void JZWindow::moveScreen(int x, int y)
{
    m_screenX = x;
    m_screenY = y;
    m_hasScreenPos = true;
    m_autoPosition = false;
    if (m_windowCreated && m_backend)
        m_backend->setScreenPos(this, x, y);
}

// ==================== 通用模态循环 ====================
// JZDialog::exec 与 popup 弹层共用。真窗口的循环体位于 JZWin32Thread (与主循环共享
// processAndPaint 轮体: 泵 Win32 消息 + 框架事件 + 脏区绘制, owner 禁用/恢复为
// DialogBoxParam 语义) —— 由后端转发; 虚拟窗口没有平台模态循环(返回 0)。
int JZWindow::execModal(bool disableOwner)
{
    return m_backend ? m_backend->execModal(this, disableOwner) : 0;
}

// ==================== 键盘 ====================

// Popup 型: Esc 隐藏 (模态循环退出); 其余交基类
void JZWindow::keyPressEvent(JZKeyEvent* event)
{
    if (m_windowType == Popup && event->key() == VK_ESCAPE) {
        hide();
        event->accept();
        return;
    }
    JZWidget::keyPressEvent(event);
}
