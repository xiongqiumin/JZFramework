// ==================== 虚拟桌面上的一个窗口 (外框 + 客户区) ====================
// 客户区是**真 JZWindow**(由 JZVirtualDevice 的窗口宿主钩子交过来) —— 渲染/输入/控件树与
// 真窗口同一条链; 外框 (标题栏/边框/按钮) 由本类绘制与命中, 交给 JZVirtualDevice 执行动作。
// 于是"虚拟窗口与真实窗口行为一致"是结构上保证的, 不是两边各写一遍去对齐。
#include "JZVirtualWindow.h"
#include "JZVirtualDevice.h"
#include "JZVirtualWindowBackend.h"
#include "JZWindow.h"
#include "JZDisplayDevice.h"
#include "JZPainter.h"
#include "JZFontMetrics.h"
#include "JZVirtualThread.h"   // 跨线程同步调用 (jzRunOnThreadSync)
#include "JZApplication.h"     // 跨线程: sendEvent(同步投递)
#include "JZThread.h"

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

// 接管客户区: 它就是"这台虚拟窗口"里跑应用的那半边 (控件树/事件/渲染全在它身上)。
JZVirtualWindow::JZVirtualWindow(JZVirtualDevice* screen, JZWindow* client)
    : m_screen(screen), m_client(client)
{
    if (m_client)
        m_clientGuard = m_client->guardPointer();   // 弱引用: 它被回收后 client() 返回 nullptr
}

JZVirtualWindow::~JZVirtualWindow()
{
    // 客户区由本类持有 (桌面收摊时一起收掉)。⚠ 它可能已经被别处删掉了(JS 侧的 GC /
    // deleteLater), 守卫能分辨 —— 那种情况下不能再 delete 一次。
    if (m_clientGuard && m_client)
        delete m_client;
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
        return kTitleBar;      // 最小化: 只剩标题栏 (客户区不参与合成)
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
        c->setWindowFocus(got);   // 与 WM_SETFOCUS/WM_KILLFOCUS 同一条路
    notifyScreen();               // 活动/非活动标题栏配色不同
}

bool JZVirtualWindow::close()
{
    JZWindow* c = client();
    return c ? c->close() : false;
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
        if (m_screen)
            setFrameGeometry(JZRect(0, 0, m_screen->desktopWidth(), m_screen->desktopHeight()));
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
    JZMouseEvent ev(type, button, global, JZPoint(), wheelDelta, modifiers);
    ev.setProgrammatic(true);        // 与 AI 合成点击同义: 不受输入蒙版屏蔽
    // ⚠ 走 jzApp->sendEvent 而不是直接 onWindowProcess: 窗口可能属于**别的线程**
    //   (JZVirtualProcess), sendEvent 会投递到那个线程并**阻塞等它处理完** ——
    //   同一线程时就是直接派发, 两条路语义一致 (同步、按顺序)。
    jzApp->sendEvent(c, &ev);
    // ⚠ 返回只代表"事件被处理了", **不代表帧已刷新**: 控件改状态时发出的脏区是排给
    //   "窗口自己的线程"由它的循环画的 (见 JZVirtualWindowBackend::onInvalidate),
    //   要拿最新帧请 clientFrame()/renderNow() (它们会等那个线程画完)。
    return ev.isAccepted();
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
    JZKeyEvent ev(key, modifiers, text);
    ev.setPress(press);
    jzApp->sendEvent(c, &ev);    // 同上: 跨线程同步投递到窗口自己的线程
    return ev.isAccepted();
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
    if (m_minimized)
        return HitNone;

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
