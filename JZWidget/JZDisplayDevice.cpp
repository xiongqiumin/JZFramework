#include <algorithm>
#include "JZDisplayDevice.h"
#include "JZWidget.h"
#include "JZPainter.h"
#include "JZPainterEvent.h"
#include "JZResizeEvent.h"
#include "JZImGuiBackend.h"
#include "JZApplication.h"
#include "JZStd.h"

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
static void enableDpiAwareness()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32)
        return;
    auto fn = (BOOL(WINAPI*)(HANDLE))GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (fn)
        fn((HANDLE)-4);   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
}

static UINT queryDpi(HWND hwnd)
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

static std::wstring utf8ToUtf16(const std::string& text)
{
    if (text.empty())
        return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], len);
    return out;
}

// 鼠标 lParam (物理像素) -> 逻辑像素
static JZPoint physicalToLogical(LPARAM lParam, float scale)
{
    int px = (int16_t)LOWORD(lParam);
    int py = (int16_t)HIWORD(lParam);
    return JZPoint((int)std::lround(px / scale), (int)std::lround(py / scale));
}

// 按窗口样式把逻辑客户区尺寸换算为窗口外框尺寸
static RECT windowRectForClient(HWND hwnd, DWORD style, int logicalW, int logicalH, float scale)
{
    unsigned physW = (unsigned)std::lround(logicalW * scale);
    unsigned physH = (unsigned)std::lround(logicalH * scale);
    RECT rc = { 0, 0, (LONG)physW, (LONG)physH };

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    UINT dpi = queryDpi(hwnd);
    auto adjustForDpi = (BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT))
        (user32 ? GetProcAddress(user32, "AdjustWindowRectExForDpi") : nullptr);
    if (adjustForDpi)
        adjustForDpi(&rc, style, FALSE, 0, dpi);
    else
        AdjustWindowRectEx(&rc, style, FALSE, 0);
    return rc;
}

// ==================== WndProc ====================
// 窗口消息处理: 鼠标/键盘 -> 逻辑像素 -> 命中检测 -> 控件树分发。
// ImGui (纯绘图后端) 不接触任何消息。
static LRESULT CALLBACK JZDisplayDeviceWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static void registerDeviceWindowClass()
{
    static bool registered = false;
    if (registered)
        return;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = JZDisplayDeviceWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
    wc.lpszClassName = L"JZDeviceWndClass";
    RegisterClassExW(&wc);
    registered = true;
}

LRESULT CALLBACK JZDisplayDeviceWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    JZDisplayDevice* self = (JZDisplayDevice*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

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
            if (self->backend())
                self->backend()->resizeSwapChain(physW, physH);
            float s = self->scale();
            self->onWindowResized((int)std::lround(physW / s), (int)std::lround(physH / s));
        }
        return 0;

    case WM_DPICHANGED:
        if (self) {
            self->updateDpiScale();
            const RECT* suggested = (const RECT*)lParam;
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;

    // ---- 鼠标: 物理像素 / m_scale -> 逻辑像素, 交给控件树分发 ----
    case WM_MOUSEMOVE:
        if (self)
            self->dispatchMouseEvent(JZMouseEvent::Move, JZMouseEvent::NoButton,
                                     physicalToLogical(lParam, self->scale()), 0);
        return 0;

    case WM_LBUTTONDOWN:
        if (self)
            self->dispatchMouseEvent(JZMouseEvent::Press, JZMouseEvent::LeftButton,
                                     physicalToLogical(lParam, self->scale()), 0);
        return 0;

    case WM_LBUTTONUP:
        if (self)
            self->dispatchMouseEvent(JZMouseEvent::Release, JZMouseEvent::LeftButton,
                                     physicalToLogical(lParam, self->scale()), 0);
        return 0;

    case WM_RBUTTONDOWN:
        if (self)
            self->dispatchMouseEvent(JZMouseEvent::Press, JZMouseEvent::RightButton,
                                     physicalToLogical(lParam, self->scale()), 0);
        return 0;

    case WM_RBUTTONUP:
        if (self)
            self->dispatchMouseEvent(JZMouseEvent::Release, JZMouseEvent::RightButton,
                                     physicalToLogical(lParam, self->scale()), 0);
        return 0;

    case WM_MBUTTONDOWN:
        if (self)
            self->dispatchMouseEvent(JZMouseEvent::Press, JZMouseEvent::MiddleButton,
                                     physicalToLogical(lParam, self->scale()), 0);
        return 0;

    case WM_MBUTTONUP:
        if (self)
            self->dispatchMouseEvent(JZMouseEvent::Release, JZMouseEvent::MiddleButton,
                                     physicalToLogical(lParam, self->scale()), 0);
        return 0;

    case WM_MOUSEWHEEL:
        if (self) {
            POINT pt = { (int16_t)LOWORD(lParam), (int16_t)HIWORD(lParam) };
            ScreenToClient(hwnd, &pt);
            float s = self->scale();
            JZPoint logical((int)std::lround(pt.x / s), (int)std::lround(pt.y / s));
            self->dispatchMouseEvent(JZMouseEvent::Wheel, JZMouseEvent::NoButton, logical,
                                     GET_WHEEL_DELTA_WPARAM(wParam) / 120);
        }
        return 0;

    // ---- 键盘 ----
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (self)
            self->dispatchKeyEvent((uint32_t)wParam, true);
        return 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (self)
            self->dispatchKeyEvent((uint32_t)wParam, false);
        return 0;

    case WM_CLOSE:
        if (self)
            self->requestClose();
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

#endif // _WIN32

JZ_OBJECT_IMPL(JZDisplayDevice, JZWidget)

std::vector<JZDisplayDevice*> JZDisplayDevice::displayList;

void JZDisplayDevice::refresh()
{
    for (auto device : displayList)
        device->redraw();
}

void JZDisplayDevice::processEvents()
{
    for (auto device : displayList)
        device->pumpMessages();
}

JZDisplayDevice::JZDisplayDevice(int w, int h, const std::string& title)
    : m_title(title)
{
    displayList.push_back(this);
    m_visible = true;
    setGeometry(0, 0, w, h);

#ifdef _WIN32
    // ---- 创建原生窗口 (窗口与消息由本类负责, ImGui 只负责绘图) ----
    enableDpiAwareness();
    m_scale = queryDpi(nullptr) / 96.0f;

    registerDeviceWindowClass();

    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rc = windowRectForClient(nullptr, style, w, h, m_scale);

    std::wstring wtitle = utf8ToUtf16(m_title);
    m_hwnd = CreateWindowExW(0, L"JZDeviceWndClass", wtitle.c_str(), style,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (m_hwnd) {
        m_backend = new JZImGuiBackend();
        unsigned physW = (unsigned)std::lround(w * m_scale);
        unsigned physH = (unsigned)std::lround(h * m_scale);
        if (!m_backend->init(this, m_hwnd, physW, physH)) {
            m_backend->destroy();
            delete m_backend;
            m_backend = nullptr;
        }
        ShowWindow((HWND)m_hwnd, SW_SHOW);
        updateDpiScale();   // 窗口所在显示器 DPI 可能与系统不同
    }
#endif
}

JZDisplayDevice::~JZDisplayDevice()
{
    if (m_backend) {
        m_backend->destroy();
        delete m_backend;
        m_backend = nullptr;
    }
#ifdef _WIN32
    if (m_hwnd) {
        DestroyWindow((HWND)m_hwnd);
        m_hwnd = nullptr;
    }
#endif
    vec_removeAll(displayList, this);
}

void JZDisplayDevice::paintEvent(JZPainterEvent* event)
{
    JZPainter pt(this);
    pt.fillRect(rect(), JZColor::White);
}

JZLayer* JZDisplayDevice::layer()
{
    return &m_layer;
}

void JZDisplayDevice::setTitle(const std::string& title)
{
    m_title = title;
#ifdef _WIN32
    if (m_hwnd)
        SetWindowTextW((HWND)m_hwnd, utf8ToUtf16(title).c_str());
#endif
}

std::string JZDisplayDevice::title() const
{
    return m_title;
}

void JZDisplayDevice::setWindowVisible(bool visible)
{
#ifdef _WIN32
    if (m_hwnd)
        ShowWindow((HWND)m_hwnd, visible ? SW_SHOW : SW_HIDE);
#endif
}

void JZDisplayDevice::setOthersInputEnabled(JZDisplayDevice* exclude, bool enabled)
{
#ifdef _WIN32
    for (auto device : displayList) {
        if (device != exclude && device->m_hwnd)
            EnableWindow((HWND)device->m_hwnd, enabled);
    }
#else
    (void)exclude; (void)enabled;
#endif
}

void JZDisplayDevice::resize(int w, int h)
{
    bool sizeChanged = (w != width() || h != height());
    JZWidget::resize(w, h);
#ifdef _WIN32
    if (sizeChanged && m_hwnd) {
        DWORD style = (DWORD)GetWindowLongPtrW((HWND)m_hwnd, GWL_STYLE);
        RECT rc = windowRectForClient((HWND)m_hwnd, style, w, h, m_scale);
        SetWindowPos((HWND)m_hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);   // WM_SIZE -> onWindowResized
    }
#endif
    if (sizeChanged)
        m_invalidRect = rect();
}

float JZDisplayDevice::scale() const
{
    return m_scale;
}

JZImGuiBackend* JZDisplayDevice::backend()
{
    return m_backend;
}

void JZDisplayDevice::updateDpiScale()
{
#ifdef _WIN32
    float s = queryDpi((HWND)m_hwnd) / 96.0f;
    if (std::fabs(s - m_scale) > 0.001f) {
        m_scale = s;
        if (m_backend)
            m_backend->rebuildFonts();   // 字号随 DPI 缩放
    }
#endif
}

void JZDisplayDevice::pumpMessages()
{
#ifdef _WIN32
    if (!m_hwnd)
        return;

    MSG msg;
    while (PeekMessageW(&msg, (HWND)m_hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
#endif
}

void JZDisplayDevice::makeInvalidate(const JZRect& rc)
{
    if (m_invalidRect.isEmpty())
        m_invalidRect = rc;
    else
        m_invalidRect = m_invalidRect.united(rc);
}

void JZDisplayDevice::redraw()
{
    if (m_invalidRect.isEmpty())
        return;
    m_invalidRect.clear();

    // ImGui 每帧全量重绘: 遍历整棵控件树收集任务 (裁剪区 = 整个设备)。
    JZRect clip = rect();

    std::vector<JZWidget*> widget_list;
    std::vector<JZRect> coverage = { clip };
    collectFrontToBack(this, clip, coverage, widget_list);

    std::reverse(widget_list.begin(), widget_list.end());
    // 统一创建 painter 并注入: 每个 widget 一次 paintEvent 对应一个 painter,
    // 产生的 JZDrawTask 追加到 m_layer, 最后由渲染后端一次性回放。
    for (auto w : widget_list)
    {
        JZPainter painter(w);
        painter.setClip(clip);

        JZPainterEvent event;
        event.setPainter(&painter);
        event.setRect(clip);
        w->event(&event);
    }

    if (m_backend)
        m_backend->render(&m_layer);
    else
        m_layer.clearTasks();
}

// ==================== 剔除遍历 ====================
// "前到后"累计不透明覆盖, 收集需要绘制的 widget, 最后反转成"后到前"绘制顺序。
void JZDisplayDevice::collectFrontToBack(JZWidget* w, const JZRect& area, std::vector<JZRect>& coverage,
                                         std::vector<JZWidget*>& out)
{
    JZRect wa = w->toGlobal(w->rect()).intersect(area);

    // 先处理子节点(前到后), 使上层子节点先贡献覆盖, 再判断本节点(本节点在子节点之后)
    // 注意: 挂在本树下的其它显示设备(如对话框窗口)不属于本可视树, 跳过
    const auto& ch = w->children();
    for (int i = (int)ch.size() - 1; i >= 0; --i) {
        auto* c = jzobject_cast<JZWidget*>(ch[i]);
        if (!c || jzobject_cast<JZDisplayDevice*>(c))
            continue;
        collectFrontToBack(c, area, coverage, out);
    }

    // 被已覆盖矩形完全包含 -> 剔除 (不发 paintEvent / 不生成 draw_task)
    bool covered = false;
    for (const JZRect& c : coverage) {
        if (wa.left()   >= c.left()  && wa.right()  <= c.right() &&
            wa.top()    >= c.top()   && wa.bottom() <= c.bottom()) {
            covered = true;
            break;
        }
    }
    {
        out.push_back(w);
        coverage.push_back(wa);
    }
}

// ==================== 输入分发 ====================

// 命中检测: 顶层可见且包含 globalPos 的最深控件 (子控件优先于父控件)
JZWidget* JZDisplayDevice::hitTest(const JZPoint& globalPos)
{
    return hitTest(this, globalPos);
}

JZWidget* JZDisplayDevice::hitTest(JZWidget* w, const JZPoint& globalPos)
{
    if (!w->isVisible())
        return nullptr;

    const auto& ch = w->children();
    for (int i = (int)ch.size() - 1; i >= 0; --i) {
        auto* c = jzobject_cast<JZWidget*>(ch[i]);
        if (!c || jzobject_cast<JZDisplayDevice*>(c))   // 跳过挂载的其它设备(对话框)
            continue;
        if (JZWidget* hit = hitTest(c, globalPos))
            return hit;
    }

    JZRect g = w->toGlobal(w->rect());
    if (g.contains(globalPos))
        return w;
    return nullptr;
}

void JZDisplayDevice::dispatchMouseEvent(JZMouseEvent::MouseType type, JZMouseEvent::Button button,
                                         const JZPoint& globalPos, int wheelDelta)
{
    JZWidget* target = nullptr;
    switch (type) {
    case JZMouseEvent::Press:
        target = hitTest(globalPos);
        m_mouseGrab = target;   // 按下后锁定目标
        break;
    case JZMouseEvent::Release:
        target = m_mouseGrab ? m_mouseGrab : hitTest(globalPos);
        m_mouseGrab = nullptr;
        break;
    case JZMouseEvent::Move:
    case JZMouseEvent::Wheel:
        target = m_mouseGrab ? m_mouseGrab : hitTest(globalPos);
        break;
    }
    if (!target)
        target = this;

    JZRect g = target->toGlobal(target->rect());
    JZPoint local(globalPos.x() - g.left(), globalPos.y() - g.top());

    JZMouseEvent me(type, button, globalPos, local, wheelDelta);
    target->event(&me);
}

void JZDisplayDevice::dispatchKeyEvent(uint32_t key, bool pressed)
{
    (void)pressed;
    JZKeyEvent ke(key);
    this->event(&ke);
}

void JZDisplayDevice::onWindowResized(int logicalW, int logicalH)
{
    if (logicalW == width() && logicalH == height())
        return;

    JZSize old = size();
    setGeometry(0, 0, logicalW, logicalH);

    JZResizeEvent ev(size(), old);
    resizeEvent(&ev);

    m_invalidRect = rect();   // 整体重绘
}

void JZDisplayDevice::requestClose()
{
    if (auto* app = JZApplication::instance())
        app->quit();
}
