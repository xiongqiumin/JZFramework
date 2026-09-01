#include "JZWindow.h"
#include "JZDisplayDevice.h"
#include "JZPainter.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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

#endif // _WIN32

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

#ifdef _WIN32
    if (m_hwnd) {
        DestroyWindow((HWND)m_hwnd);
        m_hwnd = nullptr;
    }
#endif
    delete m_device;
    m_device = nullptr;
}

// ==================== WndProc ====================
// 窗口消息处理: 鼠标/键盘 -> 逻辑像素 -> 设备分发到控件树。
// ImGui (纯绘图后端) 不接触任何消息。
#ifdef _WIN32

static LRESULT CALLBACK jzWindowWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    JZWindow* self = (JZWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
/*
    switch (msg) {
    case WM_CREATE: {
        auto* cs = (CREATESTRUCTW*)lParam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }

    case WM_SIZE:
        if (self && wParam != SIZE_MINIMIZED && lParam != 0) {
            unsigned physW = LOWORD(lParam);
            unsigned physH = HIWORD(lParam);
            if (self->device()->backend())
                self->device()->backend()->resizeSwapChain(physW, physH);
            float s = self->device()->scale();
            self->windowResized((int)std::lround(physW / s), (int)std::lround(physH / s));
        }
        return 0;

    case WM_DPICHANGED:
        if (self) {
            self->dpiChanged();
            const RECT* suggested = (const RECT*)lParam;
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;

    // ---- 鼠标: 物理像素 / scale -> 逻辑像素, 交给设备分发 ----
    case WM_MOUSEMOVE:
        if (self)
            self->device()->dispatchMouseEvent(JZMouseEvent::Move, JZMouseEvent::NoButton,
                                               jzPhysicalToLogical(lParam, self->device()->scale()), 0);
        return 0;

    case WM_LBUTTONDOWN:
        if (self)
            self->device()->dispatchMouseEvent(JZMouseEvent::Press, JZMouseEvent::LeftButton,
                                               jzPhysicalToLogical(lParam, self->device()->scale()), 0);
        return 0;

    case WM_LBUTTONUP:
        if (self)
            self->device()->dispatchMouseEvent(JZMouseEvent::Release, JZMouseEvent::LeftButton,
                                               jzPhysicalToLogical(lParam, self->device()->scale()), 0);
        return 0;

    case WM_RBUTTONDOWN:
        if (self)
            self->device()->dispatchMouseEvent(JZMouseEvent::Press, JZMouseEvent::RightButton,
                                               jzPhysicalToLogical(lParam, self->device()->scale()), 0);
        return 0;

    case WM_RBUTTONUP:
        if (self)
            self->device()->dispatchMouseEvent(JZMouseEvent::Release, JZMouseEvent::RightButton,
                                               jzPhysicalToLogical(lParam, self->device()->scale()), 0);
        return 0;

    case WM_MBUTTONDOWN:
        if (self)
            self->device()->dispatchMouseEvent(JZMouseEvent::Press, JZMouseEvent::MiddleButton,
                                               jzPhysicalToLogical(lParam, self->device()->scale()), 0);
        return 0;

    case WM_MBUTTONUP:
        if (self)
            self->device()->dispatchMouseEvent(JZMouseEvent::Release, JZMouseEvent::MiddleButton,
                                               jzPhysicalToLogical(lParam, self->device()->scale()), 0);
        return 0;

    case WM_MOUSEWHEEL:
        if (self) {
            POINT pt = { (int16_t)LOWORD(lParam), (int16_t)HIWORD(lParam) };
            ScreenToClient(hwnd, &pt);
            float s = self->device()->scale();
            JZPoint logical((int)std::lround(pt.x / s), (int)std::lround(pt.y / s));
            self->device()->dispatchMouseEvent(JZMouseEvent::Wheel, JZMouseEvent::NoButton, logical,
                                               GET_WHEEL_DELTA_WPARAM(wParam) / 120);
        }
        return 0;

    // ---- 键盘 ----
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (self)
            self->device()->dispatchKeyEvent((uint32_t)wParam, true);
        return 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (self)
            self->device()->dispatchKeyEvent((uint32_t)wParam, false);
        return 0;

    case WM_CLOSE:
        if (self)
            self->device()->requestClose();
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
*/
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

#endif // _WIN32

void JZWindow::setTitle(const std::string& title)
{
    m_title = title;
#ifdef _WIN32
    if (m_hwnd)
        SetWindowTextW((HWND)m_hwnd, jzUtf8ToUtf16(title).c_str());
#endif
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

#ifdef _WIN32
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
#endif
}

void JZWindow::showEvent(JZShowEvent* event)
{
    JZWidget::showEvent(event);

    createWindow();
#ifdef _WIN32
    if (m_hwnd)
        ShowWindow((HWND)m_hwnd, SW_SHOW);
#endif
}

void JZWindow::hideEvent(JZHideEvent* event)
{
    JZWidget::hideEvent(event);

#ifdef _WIN32
    if (m_windowCreated && m_hwnd)
        ShowWindow((HWND)m_hwnd, SW_HIDE);
#endif
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
#ifdef _WIN32
    if (changed && m_hwnd) {
        DWORD style = (DWORD)GetWindowLongPtrW((HWND)m_hwnd, GWL_STYLE);
        RECT rc = jzWindowRectForClient((HWND)m_hwnd, style, w, h, m_device->scale());
        SetWindowPos((HWND)m_hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);   // WM_SIZE -> windowResized
        update();
    }
#endif
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
#ifdef _WIN32
    m_device->setScale(jzQueryDpi((HWND)m_hwnd) / 96.0f);
#endif
}

