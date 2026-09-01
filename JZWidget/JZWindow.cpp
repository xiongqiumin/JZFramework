#include "JZWindow.h"
#include "JZDisplayDevice.h"
#include "JZPainter.h"
#include "JZImeEvent.h"

#include <algorithm>
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
// 窗口消息处理: 鼠标/键盘 -> 逻辑像素 -> 设备分发到控件树。
// ImGui (纯绘图后端) 不接触任何消息。

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

static LRESULT CALLBACK jzWindowWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    JZWindow* self = (JZWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!self)
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_CREATE: {
        auto* cs = (CREATESTRUCTW*)lParam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
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

    // ---- 鼠标: 物理像素 / scale -> 逻辑像素, 命中测试后投递给控件 ----
    case WM_MOUSEMOVE:
        self->dispatchMouse(JZMouseEvent::Move, JZMouseEvent::NoButton,
                            jzPhysicalToLogical(lParam, self->device()->scale()), 0);
        return 0;

    case WM_LBUTTONDOWN:
        self->dispatchMouse(JZMouseEvent::Press, JZMouseEvent::LeftButton,
                            jzPhysicalToLogical(lParam, self->device()->scale()), 0);
        return 0;

    case WM_LBUTTONUP:
        self->dispatchMouse(JZMouseEvent::Release, JZMouseEvent::LeftButton,
                            jzPhysicalToLogical(lParam, self->device()->scale()), 0);
        return 0;

    case WM_RBUTTONDOWN:
        self->dispatchMouse(JZMouseEvent::Press, JZMouseEvent::RightButton,
                            jzPhysicalToLogical(lParam, self->device()->scale()), 0);
        return 0;

    case WM_RBUTTONUP:
        self->dispatchMouse(JZMouseEvent::Release, JZMouseEvent::RightButton,
                            jzPhysicalToLogical(lParam, self->device()->scale()), 0);
        return 0;

    case WM_MBUTTONDOWN:
        self->dispatchMouse(JZMouseEvent::Press, JZMouseEvent::MiddleButton,
                            jzPhysicalToLogical(lParam, self->device()->scale()), 0);
        return 0;

    case WM_MBUTTONUP:
        self->dispatchMouse(JZMouseEvent::Release, JZMouseEvent::MiddleButton,
                            jzPhysicalToLogical(lParam, self->device()->scale()), 0);
        return 0;

    case WM_MOUSEWHEEL: {
        POINT pt = { (int16_t)LOWORD(lParam), (int16_t)HIWORD(lParam) };
        ScreenToClient(hwnd, &pt);
        float s = self->device()->scale();
        JZPoint logical((int)std::lround(pt.x / s), (int)std::lround(pt.y / s));
        self->dispatchMouse(JZMouseEvent::Wheel, JZMouseEvent::NoButton, logical,
                            GET_WHEEL_DELTA_WPARAM(wParam) / 120);
        return 0;
    }

    // ---- 键盘: 转发给当前焦点控件 ----
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if ((UINT)wParam == VK_PROCESSKEY)
            break;   // IME 接管, 交给 WM_IME_*
        if (self->dispatchKey((uint32_t)wParam, true, (lParam & (1u << 30)) != 0))
            return 0;
        break;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (self->dispatchKey((uint32_t)wParam, false, false))
            return 0;
        break;

    // 直接键入字符 (ASCII / 半角): IME 中文走 WM_IME_COMPOSITION, 不走这里
    case WM_CHAR: {
        if ((wchar_t)wParam == VK_PROCESSKEY)
            return 0;
        wchar_t wch = (wchar_t)wParam;
        std::string utf8 = jzUtf16ToUtf8(&wch, 1);
        if (self->dispatchChar(utf8))
            return 0;
        break;
    }

    // ---- 输入法 (IME) ----
    case WM_IME_STARTCOMPOSITION: {
        JZImeEvent ev(JZImeEvent::StartComposition);
        self->postIme(&ev);
        return 0;
    }

    case WM_IME_ENDCOMPOSITION: {
        JZImeEvent ev(JZImeEvent::EndComposition);
        self->postIme(&ev);
        return 0;
    }

    case WM_IME_NOTIFY:
        if (wParam == IMN_OPENCANDIDATE || wParam == IMN_CHANGECANDIDATE ||
            wParam == IMN_CLOSECANDIDATE) {
            JZImeEvent ev(JZImeEvent::Candidate);
            self->postIme(&ev);
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
        self->postIme(&ev);
        return 0;   // 关键: 不转发 DefWindowProc, 否则 IME 默认行为干扰
    }

    case WM_SETFOCUS:
        self->onWindowFocus(true);
        return 0;

    case WM_KILLFOCUS:
        self->onWindowFocus(false);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

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
    wc.style = CS_HREDRAW | CS_VREDRAW;
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

    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rc = jzWindowRectForClient(nullptr, style, w, h, m_device->scale());

    std::wstring wtitle = jzUtf8ToUtf16(m_title);
    m_hwnd = CreateWindowExW(0, L"JZWindowWndClass", wtitle.c_str(), style,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (m_hwnd) {
        unsigned physW = (unsigned)std::lround(w * m_device->scale());
        unsigned physH = (unsigned)std::lround(h * m_device->scale());
        m_device->init(m_hwnd, physW, physH);
    }
}

void JZWindow::showEvent(JZShowEvent* event)
{
    JZWidget::showEvent(event);

    createWindow();
    if (m_hwnd)
        ShowWindow((HWND)m_hwnd, SW_SHOW);
}

void JZWindow::hideEvent(JZHideEvent* event)
{
    JZWidget::hideEvent(event);

    if (m_windowCreated && m_hwnd)
        ShowWindow((HWND)m_hwnd, SW_HIDE);
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
        old->sendEvent(&ev);
    }
    if (w) {
        JZEvent ev(JZEvent_focusIn);
        w->sendEvent(&ev);
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
            JZWidget* deeper = hitTestRec(c, global);
            return deeper ? deeper : c;
        }
    }
    return nullptr;
}

// ==================== 输入分发 ====================
bool JZWindow::dispatchMouse(JZMouseEvent::MouseType t, JZMouseEvent::Button b,
                             const JZPoint& global, int wheelDelta)
{
    JZWidget* hit = hitTest(global);
    if (hit) {
        JZRect gr = hit->mapToGlobal(hit->rect());
        JZPoint local(global.x() - gr.left(), global.y() - gr.top());
        JZMouseEvent ev(t, b, global, local, wheelDelta);
        hit->sendEvent(&ev);
        // 左键按下: 把焦点交给命中控件 (点击切换焦点)
        if (t == JZMouseEvent::Press && b == JZMouseEvent::LeftButton)
            hit->setFocus();
    } else if (t == JZMouseEvent::Press && b == JZMouseEvent::LeftButton) {
        setFocusWidget(nullptr);   // 点空白: 清空焦点
    }
    return true;
}

bool JZWindow::dispatchKey(uint32_t key, bool press, bool repeat)
{
    if (!m_focusWidget)
        return false;
    uint32_t mods = JZKeyEvent::NoModifier;
    if (GetKeyState(VK_SHIFT)   & 0x8000) mods |= JZKeyEvent::ShiftModifier;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= JZKeyEvent::CtrlModifier;
    if (GetKeyState(VK_MENU)    & 0x8000) mods |= JZKeyEvent::AltModifier;
    JZKeyEvent ev(key, mods, std::string(), repeat);
    ev.setPress(press);
    m_focusWidget->sendEvent(&ev);
    return true;
}

bool JZWindow::dispatchChar(const std::string& text)
{
    if (!m_focusWidget)
        return false;
    uint32_t mods = JZKeyEvent::NoModifier;
    if (GetKeyState(VK_SHIFT)   & 0x8000) mods |= JZKeyEvent::ShiftModifier;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= JZKeyEvent::CtrlModifier;
    if (GetKeyState(VK_MENU)    & 0x8000) mods |= JZKeyEvent::AltModifier;
    JZKeyEvent ev(0, mods, text, false);
    ev.setPress(true);
    m_focusWidget->sendEvent(&ev);
    return true;
}

void JZWindow::postIme(JZImeEvent* ev)
{
    if (m_focusWidget)
        m_focusWidget->sendEvent(ev);
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
            m_focusWidget->sendEvent(&ev);
        }
    } else {
        if (m_focusWidget) {
            JZEvent ev(JZEvent_focusOut);
            m_focusWidget->sendEvent(&ev);
        }
        ImmAssociateContext((HWND)m_hwnd, nullptr);  // 窗口失焦: 断开 IMC
    }
}

