#include "JZWindow.h"
#include "JZDisplayDevice.h"
#include "JZPainter.h"
#include "JZPainterEvent.h"
#include "JZImeEvent.h"
#include "JZApplication.h"
#include "JZWin32Thread.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <imm.h>
#include <cmath>

// Per-Monitor V2 DPI 感知 (逻辑像素 * scale = 物理像素 的基础)
static void jzEnableDpiAwareness()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32)
        return;
    auto fn = (BOOL(WINAPI*)(HANDLE))GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (fn)
        fn((HANDLE)-4);   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
}

static UINT jzQueryDpi(HWND hwnd)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 && hwnd) {
        auto fn = (UINT(WINAPI*)(HWND))GetProcAddress(user32, "GetDpiForWindow");
        if (fn) {
            UINT dpi = fn(hwnd);
            if (dpi > 0)
                return dpi;
        }
    }
    HDC dc = GetDC(hwnd);
    UINT dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(hwnd, dc);
    return dpi > 0 ? dpi : 96;
}

static std::wstring jzUtf8ToUtf16(const std::string& text)
{
    if (text.empty())
        return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], len);
    return out;
}

// 鼠标 lParam (物理像素) -> 逻辑像素
static JZPoint jzPhysicalToLogical(LPARAM lParam, float scale)
{
    int px = (int16_t)LOWORD(lParam);
    int py = (int16_t)HIWORD(lParam);
    return JZPoint((int)std::lround(px / scale), (int)std::lround(py / scale));
}

// 按窗口样式把逻辑客户区尺寸换算为窗口外框尺寸
static RECT jzWindowRectForClient(HWND hwnd, DWORD style, int logicalW, int logicalH, float scale)
{
    unsigned physW = (unsigned)std::lround(logicalW * scale);
    unsigned physH = (unsigned)std::lround(logicalH * scale);
    RECT rc = { 0, 0, (LONG)physW, (LONG)physH };

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    UINT dpi = jzQueryDpi(hwnd);
    auto adjustForDpi = (BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT))
        (user32 ? GetProcAddress(user32, "AdjustWindowRectExForDpi") : nullptr);
    if (adjustForDpi)
        adjustForDpi(&rc, style, FALSE, 0, dpi);
    else
        AdjustWindowRectEx(&rc, style, FALSE, 0);
    return rc;
}

// 所有存活窗口 (消息泵/模态遍历用)
static std::vector<JZWindow*>& jzWindowList()
{
    static std::vector<JZWindow*> list;
    return list;
}

JZ_OBJECT_IMPL(JZWindow, JZWidget)

JZWindow::JZWindow(JZWidget* parent)
    : JZWidget(parent)
{
    // 默认尺寸
    m_device = new JZDisplayDevice();

    JZWidget::resize(800, 600);   // non-virtual: avoid ctor-time virtual dispatch

    jzWindowList().push_back(this);
}

JZWindow::JZWindow(int w, int h, const std::string& title, JZWidget* parent)
    : JZWindow(parent)
{
    resize(w, h);
    setTitle(title);
}

JZWindow::~JZWindow()
{
    std::vector<JZWindow*>& list = jzWindowList();
    list.erase(std::remove(list.begin(), list.end(), this), list.end());

    if (m_hwnd) {
        DestroyWindow((HWND)m_hwnd);
        m_hwnd = nullptr;
    }
    delete m_device;
    m_device = nullptr;
}

// ==================== WndProc ====================
// 平台翻译层: 输入类消息 (鼠标/键盘/字符/IME/窗口焦点) 翻译成 JZEvent 后,
// 统一交给 JZWindow::onWindowProcess 分发到控件树; 窗口生命周期消息
// (WM_SIZE/WM_DPICHANGED/WM_PAINT/WM_CLOSE) 不对应 JZEvent 类型, 直接处理。

// UTF-16 (wchar_t) -> UTF-8 (IME / WM_CHAR 用)
static std::string jzUtf16ToUtf8(const wchar_t* w, int len)
{
    if (!w || len <= 0)
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w, len, &out[0], n, nullptr, nullptr);
    return out;
}

// Shift/Ctrl/Alt 当前按下状态 -> JZKeyEvent::Modifier 位组合
static uint32_t jzCurrentKeyModifiers()
{
    uint32_t mods = JZKeyEvent::NoModifier;
    if (GetKeyState(VK_SHIFT)   & 0x8000) mods |= JZKeyEvent::ShiftModifier;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= JZKeyEvent::CtrlModifier;
    if (GetKeyState(VK_MENU)    & 0x8000) mods |= JZKeyEvent::AltModifier;
    return mods;
}

// 鼠标消息 -> JZMouseEvent (物理像素 -> 逻辑像素; localPos 由 dispatchMouse 命中后回填)
static bool jzSendMouseEvent(JZWindow* self, JZMouseEvent::MouseType t, JZMouseEvent::Button b,
                             LPARAM lParam, int wheelDelta = 0)
{
    JZMouseEvent ev(t, b, jzPhysicalToLogical(lParam, self->device()->scale()),
                    JZPoint(), wheelDelta);
    return self->onWindowProcess(&ev);
}

static LRESULT CALLBACK jzWindowWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_CREATE) {
        auto* cs = (CREATESTRUCTW*)lParam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }

    JZWindow* self = (JZWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!self)
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    // ---- popup 型窗口: 鼠标抓取期间, 客户区外的按下/移动 -> 关闭并吞掉 ----
    // (按下关闭同时吞掉点击, 避免点 combo 自身出现"关闭又立刻重开"的 toggle)
    if (self->windowType() == JZWindow::Popup &&
        (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN ||
         msg == WM_MOUSEMOVE)) {
        float s = self->device() ? self->device()->scale() : 1.0f;
        int px = (int16_t)LOWORD(lParam);
        int py = (int16_t)HIWORD(lParam);
        bool outside = (px < 0 || py < 0 ||
                        px >= (int)std::lround(self->width() * s) ||
                        py >= (int)std::lround(self->height() * s));
        if (outside) {
            if (msg != WM_MOUSEMOVE)
                self->hide();
            return 0;   // 吞掉
        }
    }

    switch (msg) {

    // 尺寸上下限: 对接 JZWidget 的 min/max 约束 (用户拖拽缩放也生效)
    case WM_GETMINMAXINFO: {
        DefWindowProcW(hwnd, msg, wParam, lParam);   // 先取系统默认
        if (self && self->device()) {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            float s = self->device()->scale();
            DWORD st = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
            RECT pad = { 0, 0, 0, 0 };
            HMODULE user32 = GetModuleHandleW(L"user32.dll");
            UINT dpi = jzQueryDpi(hwnd);
            auto adjustForDpi = (BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT))
                (user32 ? GetProcAddress(user32, "AdjustWindowRectExForDpi") : nullptr);
            if (adjustForDpi)
                adjustForDpi(&pad, st, FALSE, 0, dpi);
            else
                AdjustWindowRectEx(&pad, st, FALSE, 0);
            int ncx = pad.right - pad.left;   // 非客户区补偿
            int ncy = pad.bottom - pad.top;
            mmi->ptMinTrackSize.x = (int)std::lround(self->minimumWidth() * s) + ncx;
            mmi->ptMinTrackSize.y = (int)std::lround(self->minimumHeight() * s) + ncy;
            if (self->maximumWidth() < INT_MAX)
                mmi->ptMaxTrackSize.x = (int)std::lround(self->maximumWidth() * s) + ncx;
            if (self->maximumHeight() < INT_MAX)
                mmi->ptMaxTrackSize.y = (int)std::lround(self->maximumHeight() * s) + ncy;
        }
        return 0;
    }

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && lParam != 0) {
            unsigned physW = LOWORD(lParam);
            unsigned physH = HIWORD(lParam);
            self->device()->resize((int)physW, (int)physH);
            float s = self->device()->scale();
            self->windowResized((int)std::lround(physW / s), (int)std::lround(physH / s));
        }
        return 0;

    case WM_DPICHANGED:
        self->dpiChanged();
        {
            const RECT* suggested = (const RECT*)lParam;
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;

    // ---- 鼠标: 物理像素 / scale -> 逻辑像素, 翻译成 JZMouseEvent 走 onWindowProcess ----
    case WM_MOUSEMOVE:
        if (jzSendMouseEvent(self, JZMouseEvent::Move, JZMouseEvent::NoButton, lParam))
            return 0;
        break;

    case WM_LBUTTONDOWN:
        if (jzSendMouseEvent(self, JZMouseEvent::Press, JZMouseEvent::LeftButton, lParam))
            return 0;
        break;

    case WM_LBUTTONUP:
        if (jzSendMouseEvent(self, JZMouseEvent::Release, JZMouseEvent::LeftButton, lParam))
            return 0;
        break;

    case WM_RBUTTONDOWN:
        if (jzSendMouseEvent(self, JZMouseEvent::Press, JZMouseEvent::RightButton, lParam))
            return 0;
        break;

    case WM_RBUTTONUP:
        if (jzSendMouseEvent(self, JZMouseEvent::Release, JZMouseEvent::RightButton, lParam))
            return 0;
        break;

    case WM_MBUTTONDOWN:
        if (jzSendMouseEvent(self, JZMouseEvent::Press, JZMouseEvent::MiddleButton, lParam))
            return 0;
        break;

    case WM_MBUTTONUP:
        if (jzSendMouseEvent(self, JZMouseEvent::Release, JZMouseEvent::MiddleButton, lParam))
            return 0;
        break;

    case WM_MOUSEWHEEL: {
        POINT pt = { (int16_t)LOWORD(lParam), (int16_t)HIWORD(lParam) };
        ScreenToClient(hwnd, &pt);
        float s = self->device()->scale();
        JZMouseEvent ev(JZMouseEvent::Wheel, JZMouseEvent::NoButton,
                        JZPoint((int)std::lround(pt.x / s), (int)std::lround(pt.y / s)),
                        JZPoint(), GET_WHEEL_DELTA_WPARAM(wParam) / 120);
        if (self->onWindowProcess(&ev))
            return 0;
        break;
    }

    // ---- 键盘: 翻译成 JZKeyEvent, 从焦点控件起逐级向上传播 ----
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if ((UINT)wParam == VK_PROCESSKEY)
            break;   // IME 接管, 交给 WM_IME_*
        {
            JZKeyEvent ev((uint32_t)wParam, jzCurrentKeyModifiers(), std::string(),
                          (lParam & (1u << 30)) != 0);
            if (self->onWindowProcess(&ev))
                return 0;
        }
        break;

    case WM_KEYUP:
    case WM_SYSKEYUP: {
        JZKeyEvent ev((uint32_t)wParam, jzCurrentKeyModifiers(), std::string(), false);
        ev.setPress(false);
        if (self->onWindowProcess(&ev))
            return 0;
        break;
    }

    // 直接键入字符 (ASCII / 半角): 同样是 JZKeyEvent (text 非空); IME 中文走 WM_IME_COMPOSITION
    case WM_CHAR: {
        if ((wchar_t)wParam == VK_PROCESSKEY)
            return 0;
        wchar_t wch = (wchar_t)wParam;
        JZKeyEvent ev(0, jzCurrentKeyModifiers(), jzUtf16ToUtf8(&wch, 1), false);
        if (self->onWindowProcess(&ev))
            return 0;
        break;
    }

    // ---- 输入法 (IME) ----
    case WM_IME_STARTCOMPOSITION: {
        JZImeEvent ev(JZImeEvent::StartComposition);
        self->onWindowProcess(&ev);
        return 0;
    }

    case WM_IME_ENDCOMPOSITION: {
        JZImeEvent ev(JZImeEvent::EndComposition);
        self->onWindowProcess(&ev);
        return 0;
    }

    case WM_IME_NOTIFY:
        if (wParam == IMN_OPENCANDIDATE || wParam == IMN_CHANGECANDIDATE ||
            wParam == IMN_CLOSECANDIDATE) {
            JZImeEvent ev(JZImeEvent::Candidate);
            self->onWindowProcess(&ev);
        }
        return 0;

    case WM_IME_COMPOSITION: {
        JZImeEvent ev(JZImeEvent::CompStr);
        HIMC hImc = ImmGetContext(hwnd);
        if (hImc) {
            // 组字串 (未上屏)
            if (lParam & GCS_COMPSTR) {
                LONG len = ImmGetCompositionStringW(hImc, GCS_COMPSTR, nullptr, 0);
                if (len > 0) {
                    std::wstring wcomp((size_t)(len / (int)sizeof(wchar_t)), 0);
                    ImmGetCompositionStringW(hImc, GCS_COMPSTR, &wcomp[0], len);
                    ev.setCompString(jzUtf16ToUtf8(wcomp.c_str(), (int)wcomp.size()));
                } else {
                    ev.setCompString("");
                }
                ev.setFlag(JZImeEvent::CompStr);
            }
            // 每字符属性 (下标按 UTF-16)
            if (lParam & GCS_COMPATTR) {
                LONG alen = ImmGetCompositionStringW(hImc, GCS_COMPATTR, nullptr, 0);
                if (alen > 0) {
                    std::vector<unsigned char> attr((size_t)alen, 0);
                    ImmGetCompositionStringW(hImc, GCS_COMPATTR, attr.data(), alen);
                    ev.setCompAttr(attr);
                }
            }
            // 光标位置 (UTF-16 下标)
            if (lParam & GCS_CURSORPOS) {
                LONG cur = 0;
                ImmGetCompositionStringW(hImc, GCS_CURSORPOS, &cur, sizeof(LONG));
                ev.setCompCursor((int)cur);
                ev.setFlag(JZImeEvent::CursorPos);
            }
            // 结果串 (上屏)
            if (lParam & GCS_RESULTSTR) {
                LONG rlen = ImmGetCompositionStringW(hImc, GCS_RESULTSTR, nullptr, 0);
                if (rlen > 0) {
                    std::wstring wres((size_t)(rlen / (int)sizeof(wchar_t)), 0);
                    ImmGetCompositionStringW(hImc, GCS_RESULTSTR, &wres[0], rlen);
                    ev.setResultString(jzUtf16ToUtf8(wres.c_str(), (int)wres.size()));
                } else {
                    ev.setResultString("");
                }
                ev.setFlag(JZImeEvent::ResultStr);
            }
            ImmReleaseContext(hwnd, hImc);
        }
        self->onWindowProcess(&ev);
        return 0;   // 关键: 不转发 DefWindowProc, 否则 IME 默认行为干扰
    }

    // ---- 窗口焦点: 翻译成 focusIn/focusOut 事件 ----
    case WM_SETFOCUS: {
        JZEvent ev(JZEvent_focusIn);
        self->onWindowProcess(&ev);
        return 0;
    }

    case WM_KILLFOCUS: {
        JZEvent ev(JZEvent_focusOut);
        self->onWindowProcess(&ev);
        return 0;
    }

    case WM_CLOSE:
        self->requestClose();   // 走框架关闭流程 (closeEvent 可拦截, 不直接销毁)
        return 0;

    case WM_DESTROY:
        self->nativeDestroyed();
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void registerWindowClass()
{
    static bool registered = false;
    if (registered)
        return;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;   // popup/modal 类窗口带投影
    wc.lpfnWndProc = jzWindowWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
    wc.lpszClassName = L"JZWindowWndClass";
    RegisterClassExW(&wc);
    registered = true;
}

void JZWindow::setTitle(const std::string& title)
{
    m_title = title;
    if (m_hwnd)
        SetWindowTextW((HWND)m_hwnd, jzUtf8ToUtf16(title).c_str());
}

std::string JZWindow::title() const
{
    return m_title;
}

JZDisplayDevice* JZWindow::device()
{
    return m_device;
}

// 重绘指定区域: 遍历控件树(含剔除) -> 各控件 paintEvent -> backend 渲染
void JZWindow::doPaint(const JZRect& rc)
{
    if (!m_device || !m_windowCreated)
        return;
    m_device->render(this, rc);
}

// 惰性创建: 原生窗口 + 渲染设备初始化 (首次 showEvent 触发, 幂等)。
// 尺寸/标题取当前值 (构造后可能被 resize/setTitle 调整过)。
void JZWindow::createWindow()
{
    if (m_windowCreated)
        return;
    m_windowCreated = true;

    jzEnableDpiAwareness();

    int w = width(), h = height();
    m_device->setScale(jzQueryDpi(nullptr) / 96.0f);   // 初次设定 DPI 缩放

    registerWindowClass();

    DWORD style = 0;
    DWORD exStyle = 0;
    switch (m_windowType) {
    case Modal:
        style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;   // 对话框框形 (无最小/最大化)
        break;
    case Popup:
        style = WS_POPUP;
        exStyle = WS_EX_TOOLWINDOW;   // 无边框浮层, 不进任务栏
        break;
    case Normal:
    default:
        style = WS_OVERLAPPEDWINDOW;
        break;
    }
    RECT rc = jzWindowRectForClient(nullptr, style, w, h, m_device->scale());

    // 顶层窗口的 hWndParent 即 owner (DialogBoxParam 模态 / Z 序 / 共命运的基础):
    // 取父链上所属窗口 (对话框以父级所在的窗口为 owner)。
    HWND owner = nullptr;
    if (JZWidget* p = parentWidget()) {
        if (JZWindow* pw = p->window())
            owner = (HWND)pw->nativeHandle();
    }

    std::wstring wtitle = jzUtf8ToUtf16(m_title);
    m_hwnd = CreateWindowExW(exStyle, L"JZWindowWndClass", wtitle.c_str(), style,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             owner, nullptr, GetModuleHandleW(nullptr), this);
    if (m_hwnd) {
        unsigned physW = (unsigned)std::lround(w * m_device->scale());
        unsigned physH = (unsigned)std::lround(h * m_device->scale());
        m_device->init(m_hwnd, physW, physH);
        if (m_top)
            SetWindowPos((HWND)m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void JZWindow::showEvent(JZShowEvent* event)
{
    JZWidget::showEvent(event);

    createWindow();
    if (m_hwnd) {
        ShowWindow((HWND)m_hwnd, SW_SHOW);
        if (m_windowType == Popup)
            SetCapture((HWND)m_hwnd);   // 弹层: 抓取鼠标, 实现点外部关闭 (见 WndProc)
    }
}

void JZWindow::hideEvent(JZHideEvent* event)
{
    JZWidget::hideEvent(event);

    if (m_windowCreated && m_hwnd) {
        if (m_windowType == Popup)
            ReleaseCapture();
        ShowWindow((HWND)m_hwnd, SW_HIDE);
    }
}

void JZWindow::paintEvent(JZPainterEvent* event)
{
    // 窗口底色 (内容控件可覆盖)
    JZPainter p(this);
    p.fillRect(rect(), JZColor::White);
}

void JZWindow::resize(int w, int h)
{
    bool changed = (w != width() || h != height());
    JZWidget::resize(w, h);
    if (changed && m_hwnd) {
        DWORD style = (DWORD)GetWindowLongPtrW((HWND)m_hwnd, GWL_STYLE);
        RECT rc = jzWindowRectForClient((HWND)m_hwnd, style, w, h, m_device->scale());
        SetWindowPos((HWND)m_hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);   // WM_SIZE -> windowResized
        update();
    }
}

// 原生窗口尺寸变化 (WM_SIZE): 更新根控件几何 + 整体重绘
void JZWindow::windowResized(int logicalW, int logicalH)
{
    if (logicalW == width() && logicalH == height())
        return;

    JZSize old = size();
    JZWidget::resize(logicalW, logicalH);

    JZResizeEvent ev(size(), old);
    resizeEvent(&ev);

    update();
}

// 显示器 DPI 变化 (WM_DPICHANGED): 更新设备缩放 (重建字体)
void JZWindow::dpiChanged()
{
    m_device->setScale(jzQueryDpi((HWND)m_hwnd) / 96.0f);
}

// ==================== 焦点管理 ====================
void JZWindow::setFocusWidget(JZWidget* w)
{
    if (m_focusWidget == w)
        return;
    JZWidget* old = m_focusWidget;
    m_focusWidget = w;
    if (old) {
        JZEvent ev(JZEvent_focusOut);
        old->event(&ev);
    }
    if (w) {
        JZEvent ev(JZEvent_focusIn);
        w->event(&ev);
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

// ==================== 事件统一入口 ====================

// 事件队列派发入口: invalidate 在此合成到主线程 paint task (本轮 dealPaint 绘制)。
// 不再由线程扫队列抽取 —— 那依赖事件在快照前后的时序, 定时器回调里 update()
// 发出的重绘会被派发循环吞掉 (画面冻结); 走正常事件派发则任何上下文都不丢。
bool JZWindow::event(JZEvent* e)
{
    if (e->type() == JZEvent_invalidate)
    {
        JZInvalidateEvent* ev = (JZInvalidateEvent*)e;
        auto* mt = static_cast<JZWin32Thread*>(jzApp->mainThread());
        if (mt)
            mt->invalidate(this, ev->rect);
        return true;
    }
    return JZWidget::event(e);
}

// WndProc 把平台消息翻译成 JZEvent 后调到这里, 按类型分发:
//   鼠标 -> 命中测试定位目标控件;  键盘 -> 从焦点控件逐级向上传播;
//   IME  -> 焦点控件;              窗口焦点 -> onWindowFocus (IMC + 焦点事件)。
// 返回 true 表示事件已被处理, WndProc 不再转交 DefWindowProc。
bool JZWindow::onWindowProcess(JZEvent* event)
{
    switch (event->type()) {
    case JZEvent_mouse:
        return dispatchMouse((JZMouseEvent*)event);
    case JZEvent_key:
        return dispatchKey((JZKeyEvent*)event);
    case JZEvent_ime:
        dispatchIme((JZImeEvent*)event);
        return true;
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
    JZWidget* hit = hitTest(ev->globalPos());
    if (hit) {
        JZRect gr = hit->mapToGlobal(hit->rect());
        ev->setLocalPos(JZPoint(ev->globalPos().x() - gr.left(),
                                ev->globalPos().y() - gr.top()));
        hit->event(ev);
        // 左键按下: 把焦点交给命中控件 (点击切换焦点)
        if (ev->mouseType() == JZMouseEvent::Press && ev->button() == JZMouseEvent::LeftButton)
            hit->setFocus();
    } else if (ev->mouseType() == JZMouseEvent::Press && ev->button() == JZMouseEvent::LeftButton) {
        setFocusWidget(nullptr);   // 点空白: 清空焦点
    }
    return true;
}

// 键盘: 从焦点控件开始, 未被 accept 的键逐级向上 (父控件) 传播,
// 直到被接受或整条控件链都不要 -> 返回 false, 由 WndProc 转交 DefWindowProc
// (Tab / Alt 菜单等系统默认行为)。
bool JZWindow::dispatchKey(JZKeyEvent* ev)
{
    for (JZWidget* w = m_focusWidget; w; w = w->parentWidget()) {
        ev->ignore();   // 每一级重新给处理机会
        w->event(ev);
        if (ev->isAccepted())
            return true;
    }
    return false;
}

// IME: 组字/上屏/候选事件只投递给焦点控件。
void JZWindow::dispatchIme(JZImeEvent* ev)
{
    if (m_focusWidget)
        m_focusWidget->event(ev);
}

// WM_SETFOCUS / WM_KILLFOCUS: 窗口级管理 IMC, 并派发焦点进/出事件。
// 同 HWND 内多控件共享一个 IMC, 故只在窗口获/失焦时关联/断开,
// 控件间切换焦点只改 m_focusWidget (setFocusWidget), 不碰 IMC。
void JZWindow::onWindowFocus(bool got)
{
    if (got) {
        HIMC hImc = ImmGetContext((HWND)m_hwnd);
        if (hImc) {
            m_imeContext = hImc;                    // 记下默认 IMC
            ImmAssociateContext((HWND)m_hwnd, hImc); // 窗口级关联
            ImmReleaseContext((HWND)m_hwnd, hImc);
        }
        if (m_focusWidget) {
            JZEvent ev(JZEvent_focusIn);
            m_focusWidget->event(&ev);
        }
    } else {
        if (m_focusWidget) {
            JZEvent ev(JZEvent_focusOut);
            m_focusWidget->event(&ev);
        }
        ImmAssociateContext((HWND)m_hwnd, nullptr);  // 窗口失焦: 断开 IMC
    }
}

// ==================== 关闭流程 ====================
// WM_CLOSE -> 框架关闭流程: 触发 closeEvent, 重写方以 accept/ignore 表态,
// 真正的关闭动作 (销毁原生窗口) 统一在这里执行。返回是否真的关闭。
// (JZDialog::closeEvent 会 ignore() 并转为 reject: 只隐藏、不销毁, 可复用。)
bool JZWindow::requestClose()
{
    JZCloseEvent ev;
    closeEvent(&ev);
    if (ev.isAccepted() && m_hwnd)
        DestroyWindow((HWND)m_hwnd);   // -> WM_DESTROY -> nativeDestroyed
    return ev.isAccepted();
}

// 原生窗口已销毁 (WM_DESTROY): 清宿主状态避免句柄悬空;
// 之后 show() 经 createWindow 会重新创建。
void JZWindow::nativeDestroyed()
{
    m_hwnd = nullptr;
    m_windowCreated = false;
    m_focusWidget = nullptr;
}

// ==================== 窗口属性 ====================

// 置顶: 已创建的原生窗口立即生效, 未创建的记录标记待创建时套用
void JZWindow::setTop(bool on)
{
    if (m_top == on)
        return;
    m_top = on;
    if (m_hwnd)
        SetWindowPos((HWND)m_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void JZWindow::showMinimized()
{
    if (m_hwnd)
        ShowWindow((HWND)m_hwnd, SW_MINIMIZE);   // -> WM_SIZE (SIZE_MINIMIZED 被忽略)
}

void JZWindow::showMaximized()
{
    if (m_hwnd)
        ShowWindow((HWND)m_hwnd, SW_MAXIMIZE);
}

void JZWindow::showNormal()
{
    if (m_hwnd)
        ShowWindow((HWND)m_hwnd, SW_RESTORE);
}

// 屏幕逻辑坐标定位 (popup 等自定位窗口): 物理像素 = 逻辑 * scale
void JZWindow::moveScreen(int x, int y)
{
    if (!m_hwnd || !m_device)
        return;
    float s = m_device->scale();
    SetWindowPos((HWND)m_hwnd, nullptr,
                 (int)std::lround(x * s), (int)std::lround(y * s),
                 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// 控件坐标 -> 屏幕逻辑坐标: 树内累加到窗口根, ClientToScreen + DPI 换算。
// 窗口未创建/非 Win32 时退化为树内坐标。
JZPoint JZWidget::mapToScreen(const JZPoint& pos) const
{
    JZWindow* win = window();
    if (!win || !win->nativeHandle() || !win->device())
        return mapToGlobal(pos);
    JZPoint inWin = mapTo(win, pos);
    float s = win->device()->scale();
    POINT pt = { (int)std::lround(inWin.x() * s), (int)std::lround(inWin.y() * s) };
    ClientToScreen((HWND)win->nativeHandle(), &pt);
    return JZPoint((int)std::lround(pt.x / s), (int)std::lround(pt.y / s));
}

// ==================== 通用模态循环 ====================
// JZDialog::exec 与 popup 弹层共用: 可见期间泵 Win32 消息 + 框架事件 + 渲染,
// hide() 即退出。owner 禁用/恢复 (DialogBoxParam 语义; disableOwner=false
// 供下拉弹层用 — 主窗口保持可用, 外点由弹层鼠标抓取吞掉)。
int JZWindow::execModal(bool disableOwner)
{
    HWND owner = m_hwnd ? GetWindow((HWND)m_hwnd, GW_OWNER) : nullptr;
    bool ownerDisabled = (disableOwner && owner && IsWindowEnabled(owner));
    if (ownerDisabled)
        EnableWindow(owner, FALSE);

    bool quit = false;
    MSG msg;
    while (!quit && isVisible() && m_hwnd)
    {
        while (!quit && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)   // 应用退出: 归还消息并结束
            {
                PostQuitMessage((int)msg.wParam);
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit || !isVisible() || !m_hwnd)
            break;

        jzApp->processEvents();   // 框架事件队列 + 到期定时器
        doPaint(rect());
        MsgWaitForMultipleObjectsEx(0, nullptr, 30, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    if (ownerDisabled)
    {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    return 0;
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

