// ==================== 虚拟桌面上的一个窗口 (外框 + 客户区) ====================
// 客户区是**真 JZWindow**(由 JZVirtualDevice 的窗口宿主钩子交过来) —— 渲染/输入/控件树与
// 真窗口同一条链; 外框 (标题栏/边框/按钮) 由本类绘制与命中, 交给 JZVirtualDevice 执行动作。
// 于是"虚拟窗口与真实窗口行为一致"是结构上保证的, 不是两边各写一遍去对齐。
#include "JZVirtualWindow.h"
#include "JZVirtualDevice.h"
#include "JZVirtualScreenView.h"   // 输入法: 位置/组字取消要转给宿主那块屏幕控件
#include "JZVirtualWindowBackend.h"
#include "JZWindow.h"
#include "JZDisplayDevice.h"
#include "JZPainter.h"
#include "JZFontMetrics.h"
#include "JZVirtualThread.h"   // 跨线程同步调用 (jzRunOnThreadSync)
#include "JZApplication.h"     // 跨线程: sendEvent(同步投递)
#include "JZThread.h"
#include "JZDebug.h"           // jzWarning (客户区不在本线程时的兜底提示)

#include <algorithm>
#include <memory>

namespace {

const JZColor kTitleActive(0x2b, 0x57, 0x9a, 255);
const JZColor kTitleInactive(0xe4, 0xe4, 0xe4, 255);
const JZColor kTextActive(0xff, 0xff, 0xff, 255);
const JZColor kTextInactive(0x50, 0x50, 0x50, 255);
const JZColor kBorderActive(0x1f, 0x3f, 0x74, 255);
const JZColor kBorderInactive(0xb0, 0xb0, 0xb0, 255);

} // namespace

// 全局诞生序号: 窗口可能由**不同线程**(虚拟进程)建出来, 所以用原子自增 —— 它只决定
// 任务栏按钮的稳定顺序 (见 creationSeq), 与 z 序无关。
std::atomic<int> JZVirtualWindow::s_nextSeq{ 0 };

// 接管客户区: 它就是"这台虚拟窗口"里跑应用的那半边 (控件树/事件/渲染全在它身上)。
JZVirtualWindow::JZVirtualWindow(JZVirtualDevice* screen, JZWindow* client)
    : m_screen(screen), m_client(client), m_seq(s_nextSeq.fetch_add(1))
{
    if (m_client)
        m_clientGuard = m_client->guardPointer();   // 弱引用: 它被回收后 client() 返回 nullptr
}

JZVirtualWindow::~JZVirtualWindow()
{
    // 客户区由本类持有 (桌面收摊时一起收掉)。但"这一句能不能删它"有两个前提:
    //   ① 它可能已经被别处删掉了 (JS 侧的 GC / deleteLater): 守卫能分辨, 那种情况不能再删一次
    //      (守卫空 = 那个对象已经析构过)。
    //   ② 它可能**活在别的线程**上 (虚拟进程里 JS 建出来的窗口): 对象的销毁只能在它自己的线程上
    //      做 —— JZObject::clearGuardPointer 会断言 "m_thread->id() == 当前线程", 跨线程 delete
    //      直接踩断言 (实测: 在主线程点进程窗口的关闭按钮, 主线程 delete 进程线程的 JZWindow)。
    //      这种客户区交给它的线程收尾: 点关闭走 JZVirtualDevice::destroyWindow 的**投递**,
    //      进程退出时由那份引擎的 GC 在自己的线程上回收 —— 那时这里(别处)先删也已经安全,
    //      因为守卫分辨得出来 (见 ①)。
    if (m_clientGuard && m_client)
    {
        // 判据与 JZObject::clearGuardPointer 的断言同源: 记了线程归属就得是它自己的线程
        // (没记 = 无亲和, 同"主线程初始化"那类对象, 可以删)。
        JZThread* clientThread = m_client->thread();
        if (!clientThread || clientThread == JZThread::currentThread())
            delete m_client;
        else
            jzWarning() << "[vdesk] 客户区不在本线程, 留给它的线程回收 (客户区句柄: "
                        << (void*)m_client << ")";
    }
    m_client = nullptr;
}

JZWindow* JZVirtualWindow::client() const
{
    // 守卫空 = 客户区对象已经没了 (哪怕 m_client 这个裸指针还留着) ⇒ 视同没有客户区
    return m_clientGuard ? m_client : nullptr;
}

void* JZVirtualWindow::nativeHandle() const
{
    JZWindow* c = client();
    return c ? c->nativeHandle() : nullptr;
}

std::string JZVirtualWindow::title() const
{
    JZWindow* c = client();
    return c ? c->title() : std::string();
}

void JZVirtualWindow::setTitle(const std::string& title)
{
    if (JZWindow* c = client())
        c->setTitle(title);      // 标题栏画的就是它 -> 后端会通知桌面重画
    notifyScreen();
}

// ==================== 外框几何 ====================
int JZVirtualWindow::frameWidth() const
{
    JZWindow* c = client();
    return (c ? c->width() : 0) + 2 * kBorder;
}

int JZVirtualWindow::frameHeight() const
{
    if (m_minimized)
        return kTitleBar;      // 最小化: 桌面上不画它, 几何仍记着"标题栏那么大"的落点
    JZWindow* c = client();
    return kTitleBar + (c ? c->height() : 0) + kBorder;
}

JZRect JZVirtualWindow::frameGeometry() const
{
    return JZRect(m_x, m_y, frameWidth(), frameHeight());
}

void JZVirtualWindow::setFramePos(int x, int y)
{
    m_autoPosition = false;     // 外部给了坐标: 不再自动摆位
    if (m_x == x && m_y == y)
        return;
    m_x = x;
    m_y = y;
    notifyScreen();             // 坐标换了: 桌面重新合成 (客户区内容没变, 不用重绘)
}

void JZVirtualWindow::setFrameGeometry(const JZRect& rc)
{
    m_x = rc.left();
    m_y = rc.top();
    if (m_minimized) {
        notifyScreen();
        return;
    }

    int cw = rc.width() - 2 * kBorder;
    int ch = rc.height() - kTitleBar - kBorder;
    if (cw < kMinClientW)
        cw = kMinClientW;
    if (ch < kMinClientH)
        ch = kMinClientH;

    JZWindow* c = client();
    if (c && (cw != c->width() || ch != c->height()))
        c->resize(cw, ch);      // -> 后端重建离屏缓冲 + 重绘 + 通知桌面
    else
        notifyScreen();
}

// 设客户区尺寸 (外框坐标不动)。"外框 = 客户区 + 装饰"的换算只留在这里 —— 外面 (HTTP 接口/
// 自动化脚本) 说"客户区多大"就行, 不必抄 kBorder/kTitleBar 那两个常量 (抄了就会漂)。
void JZVirtualWindow::setClientSize(int clientW, int clientH)
{
    setFrameGeometry(JZRect(m_x, m_y,
                            clientW + 2 * kBorder,
                            clientH + kTitleBar + kBorder));
}

JZRect JZVirtualWindow::clientGeometry() const
{
    JZWindow* c = client();
    return JZRect(clientX(), clientY(), c ? c->width() : 0, c ? c->height() : 0);
}

// ==================== 坐标换算 (本窗口唯一口径) ====================
JZPoint JZVirtualWindow::clientToScreen(const JZPoint& pt) const
{
    return JZPoint(pt.x() + clientX(), pt.y() + clientY());
}

JZPoint JZVirtualWindow::screenToClient(const JZPoint& pt) const
{
    return JZPoint(pt.x() - clientX(), pt.y() - clientY());
}

// ==================== 显隐 / 活动态 ====================
void JZVirtualWindow::applyAutoPosition()
{
    if (!m_autoPosition || !m_screen)
        return;
    m_autoPosition = false;
    JZWindow* c = client();
    // 尺寸此刻才是最终的 (JS 可能刚 setGeometry 完) -> 现在算居中层叠才准
    const JZPoint p = m_screen->defaultWindowPos(c ? c->width() : 0, c ? c->height() : 0);
    m_x = p.x();
    m_y = p.y();
}

void JZVirtualWindow::show()
{
    // "摆位 + 出首帧 + 置顶/聚焦"全在平台后端的 show 里 (那才是"让窗口出现"这个平台动作);
    // 这里只把客户区显示出来 ⇒ JS 里 `win.show()` 与桌面 createWindow() 完全同一条路。
    if (JZWindow* c = client())
        c->show();
}

void JZVirtualWindow::hide()
{
    if (JZWindow* c = client())
        c->hide();
    notifyScreen();
}

bool JZVirtualWindow::isVisible() const
{
    JZWindow* c = client();
    return c && c->isVisible();
}

bool JZVirtualWindow::isActive() const
{
    JZWindow* c = client();
    return c && c->isActiveWindow();
}

void JZVirtualWindow::activate()
{
    if (m_screen)
        m_screen->activate(this);   // 桌面管理器: 置顶 + 键盘焦点
}

void JZVirtualWindow::setFocus(bool got)
{
    if (JZWindow* c = client())
    {
        // 客户区在**本线程**: 直接派发 (与以前一样, 顺序确定)。
        // 客户区在**别的线程** (虚拟进程建的窗口): 异步投过去, **不等** ——
        //   · "设焦点"是**通知**不是同步请求 (真系统里 WM_SETFOCUS 也是投递的, 不是 SendMessage);
        //   · 同步等它就是互等: 实测(用例从 2 秒变成永久挂住) 进程窗口 show 里给旧窗口发焦点
        //     事件(旧窗口在主线程) 同时主线程正给这个进程注入输入 -> 两个线程互相等对方处理
        //     自己投出去的那一条 (JZApplication::sendEvent 跨线程是 postEventAndWait)。
        //   顺序仍然对: 同一目标线程的事件队列是 FIFO, 这条焦点事件先于之后投给同一个窗口的
        //   输入事件被处理。
        JZThread* ct = c->thread();
        if (!ct || ct == JZThread::currentThread()) {
            c->setWindowFocus(got);   // 与 WM_SETFOCUS/WM_KILLFOCUS 同一条路
        } else {
            JZObjectPointer guard = c->guardPointer();     // 异步跑时客户区可能已经没了
            runOnObject(guard, [guard, got]() {
                if (JZWindow* win = (JZWindow*)guard.object())
                    win->setWindowFocus(got);
            });
        }
    }
    notifyScreen();               // 活动/非活动标题栏配色不同
}

// == PostMessage(hwnd, WM_CLOSE): 把"关"交给窗口自己 (见头注释)。
bool JZVirtualWindow::close()
{
    JZWindow* c = client();
    if (!c)
        return false;
    if (c->thread() == JZThread::currentThread())
        return closeLocal();          // 本线程: 直接派发, 返回"应用接受了吗"

    // 别的线程: 投过去, 不等 (同 PostMessage 拿不到答复)。
    // 判活: 客户区守卫 (它没了就没得关) + 桌面那边"窗口还在表里吗" (可能已被收走)。
    JZObjectPointer clientGuard = c->guardPointer();
    JZObjectPointer deskGuard = m_screen ? m_screen->guardPointer() : JZObjectPointer();
    JZVirtualWindow* self = this;
    runOnThread(m_process, [self, clientGuard, deskGuard]() {
        JZVirtualDevice* desk = (JZVirtualDevice*)deskGuard.object();
        if (!desk || !clientGuard.object())
            return;
        if (!desk->hasWindow(self))     // 已经不在桌面上了 (被收走/已关过): 不再动它
            return;
        self->closeLocal();
    });
    return true;
}

// 本线程的关窗: 派发 closeEvent (应用可 ignore) -> 接受才 hide + 从桌面摘下来。
// 客户区对象**保留** (它归 JS; 之后 show() 会把它重新挂回桌面)。
bool JZVirtualWindow::closeLocal()
{
    JZWindow* c = client();
    if (!c)
        return false;
    if (!c->close())                  // 应用 ignore() 拒绝了: 窗口留在原处
        return false;
    if (m_screen)
        m_screen->detachWindow(this); // "销毁平台窗口" 在虚拟桌面上 = 从桌面摘掉
    return true;
}

// ==================== 窗口状态 ====================
void JZVirtualWindow::setMinimized(bool on)
{
    if (m_minimized == on)
        return;
    m_minimized = on;
    if (on)
        m_maximized = false;   // 最小化优先
    notifyScreen();
}

void JZVirtualWindow::setMaximized(bool on)
{
    if (m_maximized == on)
        return;
    JZWindow* c = client();
    if (on) {
        m_restoreX = m_x;
        m_restoreY = m_y;
        m_restoreW = c ? c->width() : 0;
        m_restoreH = c ? c->height() : 0;
        m_maximized = true;
        m_minimized = false;
        // 铺到**工作区** (桌面减掉任务栏): 最大化不盖住任务栏/开始菜单 (同真实系统)
        if (m_screen)
            setFrameGeometry(m_screen->workArea());
        return;
    }
    m_maximized = false;
    setFrameGeometry(JZRect(m_restoreX, m_restoreY,
                            m_restoreW + 2 * kBorder,
                            m_restoreH + kTitleBar + kBorder));
}

// ==================== 渲染 / 取帧 ====================
// 本线程直接重绘 (调用方保证是窗口自己的线程)
void JZVirtualWindow::renderNowLocal()
{
    JZWindow* c = client();
    if (!c || !c->isWindowCreated())
        return;
    c->doPaint(c->rect());   // 与真窗口同一条重绘入口; 画完后端会把帧推给桌面
}

void JZVirtualWindow::renderNow()
{
    // 跨线程: 让窗口自己的线程画 (它才是帧缓冲的主人)
    if (m_process && JZThread::currentThread() != m_process)
    {
        jzRunOnThreadSync(m_process, [this]() { renderNowLocal(); }, 2000);
        return;
    }
    renderNowLocal();
}

// 本线程直接读帧缓冲 (调用方保证: 要么就是窗口自己的线程, 要么它是同线程窗口)
JZImage JZVirtualWindow::clientFrameLocal()
{
    JZImage img;
    JZWindow* c = client();
    if (c && c->device())
        c->device()->snapshot(img);
    return img;
}

// 取帧: 跨线程时**让窗口自己的线程**拷一份 (runSyncOnThread) —— 与它自己的绘制天然互斥,
// 管理器这边只拿一份拷贝, 绝不直接碰别人的帧缓冲。超时/线程已停时返回空图 (那一帧先跳过)。
JZImage JZVirtualWindow::clientFrame()
{
    if (m_process && JZThread::currentThread() != m_process)
    {
        // ⚠ 结果放 shared_ptr: 万一超时, 那个 lambda 之后仍可能执行, 不能捕调用方的栈引用
        auto out = std::make_shared<JZImage>();
        if (jzRunOnThreadSync(m_process, [this, out]() { *out = clientFrameLocal(); }, 2000))
            return *out;
        return JZImage();     // 超时/线程不在: 不阻塞桌面, 这一帧留空
    }
    return clientFrameLocal();
}

// 帧缓存: 内容变化时由窗口自己的线程刷新 (见 JZVirtualDevice::onWindowFrameChanged)。
// ⚠ 别的线程调进来时**不能**读帧缓冲 (那是窗口线程的), 只记脏等它自己刷。
bool JZVirtualWindow::captureFrame()
{
    if (m_process && JZThread::currentThread() != m_process)
    {
        m_frameDirty = true;
        return false;
    }
    m_frame = clientFrameLocal();
    m_frameDirty = false;
    m_frameSeq.fetch_add(1, std::memory_order_relaxed);   // 合成侧据此判"内容变了" (见 frameSeq)
    return true;
}

JZImage JZVirtualWindow::snapshot(bool renderFirst)
{
    if (renderFirst)
        renderNow();
    return clientFrame();
}

// ==================== 输入注入 (客户区坐标) ====================
// ⚠ globalPos 必须是**桌面坐标** —— 否则与真实路径(平台消息层给屏幕坐标)口径不一致,
//   hitTest / enter-leave / localPos 全都会错位 (见 JZVirtualWindowBackend::clientToScreen)。
bool JZVirtualWindow::injectMouse(JZMouseEvent::MouseType type, JZMouseEvent::Button button,
                                  int x, int y, int wheelDelta, uint32_t modifiers)
{
    JZWindow* c = client();
    if (!c)
        return false;
    const JZPoint global = clientToScreen(JZPoint(x, y));
    JZMouseEvent* ev = new JZMouseEvent(type, button, global, JZPoint(), wheelDelta, modifiers);
    ev->setProgrammatic(true);       // 与 AI 合成点击同义: 不受输入蒙版屏蔽
    return postToClient(c, ev);       // 投递 (== PostMessage; 见头注释)
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
    JZWindow* c = client();
    if (!c)
        return false;
    JZKeyEvent* ev = new JZKeyEvent(key, modifiers, text);
    ev->setPress(press);
    return postToClient(c, ev);   // 投递 (== PostMessage; 见头注释)
}

// 一条消息的去向: 本线程直接派发 (你就是它的事件循环), 别的线程 postEvent 异步投递。
// ⚠ sendEvent 不接管所有权 / postEvent 接管 (目标线程派发完自己删) —— 差异只在这里。
bool JZVirtualWindow::postToClient(JZWindow* c, JZEvent* ev)
{
    if (!c || !ev)
        return false;
    if (c->thread() == JZThread::currentThread()) {
        jzApp->sendEvent(c, ev);
        const bool accepted = ev->isAccepted();
        delete ev;
        return accepted;              // 本线程: 返回"应用接受/处理了吗"
    }
    jzApp->postEvent(c, ev);          // 跨线程: 投出去就返回 (同 PostMessage)
    return true;
}

// 按 UTF-8 逐字符输入: 每个字符拆成 press + release 两次键事件 (与真键盘同序, 控件侧
// "按下插字、抬起收尾" 的行为都能跑到)。非可打印内容 (组合键/IME) 走 injectKey/ImeEvent。
bool JZVirtualWindow::injectText(const std::string& utf8)
{
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

// 输入法事件: 与字符输入分开走 (真窗口里 WM_IME_COMPOSITION 也不是 WM_CHAR)。
// 目标 = 客户区的焦点控件 (JZWindow::dispatchIme), 由它自绘组字串并上屏。
bool JZVirtualWindow::injectIme(const JZImeEvent& src)
{
    JZWindow* c = client();
    if (!c)
        return false;
    // 拷一份再投: 源那条消息是平台层给的栈上对象而 postEvent 那条路会接管所有权并删除
    JZImeEvent* ev = new JZImeEvent(src);
    ev->ignore();                  // 键盘/IME 的规矩: 默认未处理, 被 accept 才算消费
    return postToClient(c, ev);
}

// 是否还需要输入法: 突变了才通知宿主重算 IMC (见 setImeWanted 的说明)
void JZVirtualWindow::setImeWanted(bool on)
{
    if (m_imeWanted.exchange(on) == on)
        return;
    if (m_screen)
        m_screen->notifyImeChanged();
}

// 组字窗/候选窗位置 (桌面坐标) -> 桌面 -> 宿主那块显示控件 (跨线程: 转投到它的线程上)
void JZVirtualWindow::setImeWindowPos(const JZRect& lineRect, const JZPoint& candPos)
{
    if (!m_screen)
        return;
    m_screen->runOnView([lineRect, candPos](JZVirtualScreenView* v) {
        v->setImeWindowPos(lineRect, candPos);
    });
}

// 不再需要输入法 (焦点离开文本控件 / 窗口失焦): 把宿主 IMC 里没上屏的组字丢掉,
// 免得换了个虚拟窗口却还在组上一段字。
void JZVirtualWindow::cancelHostIme()
{
    if (!m_screen)
        return;
    m_screen->runOnView([](JZVirtualScreenView* v) {
        if (JZWindow* host = v->window())
            host->cancelImeComposition();
    });
}

// ==================== 窗口管理 (JZVirtualDevice 用) ====================
JZRect JZVirtualWindow::buttonRect(int index) const
{
    // 从右往左: 0=最小化, 1=最大化, 2=关闭
    const int right = m_x + frameWidth() - kBorder - 4;
    const int x = right - (2 - index) * (kButton + kButtonGap) - kButton;
    const int y = m_y + (kTitleBar - kButton) / 2;
    return JZRect(x, y, kButton, kButton);
}

JZVirtualWindow::HitRegion JZVirtualWindow::hitTest(int screenX, int screenY) const
{
    if (!isVisible())
        return HitNone;
    // 最小化: 桌面上没有它 (不合成/不画), 也不可能被点到 —— 还原只能走任务栏按钮或
    // 平台 API (与真实系统一致: 最小化窗口不占屏幕、不参与命中)。
    if (m_minimized)
        return HitNone;
    const JZRect f = frameGeometry();
    if (screenX < f.left() || screenX > f.right() ||
        screenY < f.top() || screenY > f.bottom())
        return HitNone;

    // 标题栏按钮优先
    for (int i = 0; i < 3; ++i) {
        if (buttonRect(i).contains(screenX, screenY))
            return (i == 0) ? HitMinimize : (i == 1) ? HitMaximize : HitClose;
    }

    // 边框 / 四角 (缩放手柄; 角优先)
    const bool L = screenX < f.left() + kBorder;
    const bool R = screenX >= f.right() + 1 - kBorder;
    const bool T = screenY < f.top() + kBorder;
    const bool B = screenY >= f.bottom() + 1 - kBorder;
    if (L && T) return HitBorderTopLeft;
    if (R && T) return HitBorderTopRight;
    if (L && B) return HitBorderBottomLeft;
    if (R && B) return HitBorderBottomRight;
    if (L) return HitBorderLeft;
    if (R) return HitBorderRight;
    if (T) return HitBorderTop;
    if (B) return HitBorderBottom;

    if (screenY < f.top() + kTitleBar)
        return HitTitle;                  // 标题栏 (拖动窗口)

    const JZRect c = clientGeometry();
    if (c.contains(screenX, screenY))
        return HitClient;
    return HitNone;
}

JZCursorShape JZVirtualWindow::cursorShapeAt(int screenX, int screenY) const
{
    switch (hitTest(screenX, screenY)) {
    case HitBorderLeft:
    case HitBorderRight:       return JZCursorShape::SizeHor;
    case HitBorderTop:
    case HitBorderBottom:      return JZCursorShape::SizeVer;
    case HitBorderTopLeft:
    case HitBorderBottomRight: return JZCursorShape::SizeFDiag;
    case HitBorderTopRight:
    case HitBorderBottomLeft:  return JZCursorShape::SizeBDiag;
    default:                   return JZCursorShape::Arrow;
    }
}

// 画外框: 边框底色 -> 标题栏 -> 标题 -> 三个按钮。坐标 = 桌面坐标 (合成时直接落在桌面上)。
void JZVirtualWindow::paintDecoration(JZPainter& p, bool active) const
{
    const JZColor titleBg = active ? kTitleActive : kTitleInactive;
    const JZColor textCol = active ? kTextActive : kTextInactive;
    const JZColor borderCol = active ? kBorderActive : kBorderInactive;
    const int fx = m_x, fy = m_y;
    const int fw = frameWidth(), fh = frameHeight();

    // 边框底色 (客户区/标题栏随后覆盖其内部)
    p.fillRect(fx, fy, fw, fh, borderCol);
    // 标题栏
    p.fillRect(fx + kBorder, fy + kBorder, fw - 2 * kBorder, kTitleBar - kBorder, titleBg);

    // 标题 (居中偏左, 到按钮区为止)
    JZFont font(12);
    p.setFont(font);
    p.setPen(JZPen(textCol, 1));
    const int tx = fx + kBorder + 6;
    p.drawText(tx, fy + kTitleBar / 2, title().c_str());

    // 三个按钮 (只画字形, 底色就是标题栏)
    for (int i = 0; i < 3; ++i) {
        const JZRect r = buttonRect(i);
        const int cx = r.left() + r.width() / 2;
        const int cy = r.top() + r.height() / 2;
        p.setPen(JZPen(textCol, 1));
        if (i == 0) {                       // 最小化: 一条横线
            p.drawLine(r.left() + 2, cy + 2, r.right() - 2, cy + 2);
        } else if (i == 1) {                // 最大化: 一个小方框
            p.drawRect(r.left() + 2, r.top() + 2, r.width() - 4, r.height() - 4);
        } else {                            // 关闭: 交叉
            p.drawLine(r.left() + 3, r.top() + 3, r.right() - 3, r.bottom() - 3);
            p.drawLine(r.right() - 3, r.top() + 3, r.left() + 3, r.bottom() - 3);
        }
    }
}

void JZVirtualWindow::notifyScreen()
{
    if (m_screen)
        m_screen->onWindowFrameChanged(this);
}

// 桌面 DPI 缩放变了 (JZVirtualDevice::setScale 逐个窗口调):
// 客户区的设备换缩放到新 DPI, 帧缓冲按"客户区逻辑尺寸 * scale"重建, 再重画一帧。
// ⚠ 帧缓冲(设备)属于**窗口自己的线程**: "换缩放 + 重建 + 重画"三件事都必须在那一个线程上做 ——
//   在别的线程上 resize, 会和那个线程正在跑的绘制/取帧撞上 (实测: 主线程 resize 的同时进程
//   线程 snapshot -> 读已经重建掉的位图数据, AV; 这个竞态在输入还是同步投递时就偶发, 输入改成
//   投递之后几乎必现)。所以这里跨线程时**投过去并等它做完** (那个 lambda 不回等调用方 ⇒ 不互等)。
// ⚠ 调用方(桌面)必须先松开 m_mutex —— 这一句会等窗口线程, 而它可能正回头调桌面。
void JZVirtualWindow::applyDeviceScale()
{
    if (m_process && JZThread::currentThread() != m_process) {
        JZObjectPointer guard = m_clientGuard;    // 投递期间客户区(乃至外框)可能没了: 判活
        jzRunOnThreadSync(m_process, [this, guard]() {
            if (guard.object())
                applyDeviceScaleLocal();
        }, 2000);
        return;
    }
    applyDeviceScaleLocal();
}

// 本线程的那一半 (调用方保证: 要么就是窗口自己的线程, 要么它是同线程窗口)
void JZVirtualWindow::applyDeviceScaleLocal()
{
    JZWindow* c = client();
    if (!m_screen || !c || !c->device() || !c->isWindowCreated())
        return;                     // 还没建帧缓冲: create() 时会直接按新 scale 建
    const float s = m_screen->scale();
    c->device()->setScale(s);
    c->device()->resize(m_screen->toPhysical(c->width()), m_screen->toPhysical(c->height()));
    renderNowLocal();               // 重画 (帧缓冲的主人只有一个线程, 见 renderNow)
}
