#include "JZWindow.h"
#include "JZDisplayDevice.h"
#include "JZPainter.h"
#include "JZPainterEvent.h"
#include "JZImeEvent.h"
#include "JZApplication.h"
#include "JZWin32Thread.h"
#include "JZScreen.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <fstream>
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

// DPI 查询的唯一入口: 屏幕级实现统一在 JZScreen (GetDpiForMonitor 优先,
// 逐级回落 GetDpiForWindow / 主屏 GDI)。hwnd 为 nullptr 时取主屏 DPI。
// 进程级 Per-Monitor V2 感知声明已由 JZWin32Thread 构造完成
// (SetProcessDpiAwarenessContext) —— 逻辑像素 * scale = 物理像素 的基础。
static UINT jzQueryDpi(HWND hwnd)
{
    return (UINT)std::lround(JZScreen::logicalDpiFor(hwnd));
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

// 光标形状 -> Windows 标准光标句柄 (系统内建, LoadCursorW 幂等可复用)
// 注: 非 UNICODE 构建下 IDC_* 是 MAKEINTRESOURCE(LPSTR), 不能赋给 LPCWSTR,
// 这里直接用系统标准光标的资源 ID (与 winuser.h 的 IDC_* 常量一致) 配宽版宏。
static HCURSOR jzCursorHandle(JZCursorShape shape)
{
    int id = 32512;   // IDC_ARROW
    switch (shape) {
    case JZCursorShape::IBeam:        id = 32513; break;   // IDC_IBEAM
    case JZCursorShape::PointingHand: id = 32649; break;   // IDC_HAND
    case JZCursorShape::SizeHor:      id = 32644; break;   // IDC_SIZEWE
    case JZCursorShape::SizeVer:      id = 32645; break;   // IDC_SIZENS
    case JZCursorShape::SizeAll:      id = 32646; break;   // IDC_SIZEALL
    case JZCursorShape::SizeFDiag:    id = 32642; break;   // IDC_SIZENWSE
    case JZCursorShape::SizeBDiag:    id = 32643; break;   // IDC_SIZENESW
    case JZCursorShape::Cross:        id = 32515; break;   // IDC_CROSS
    case JZCursorShape::Forbidden:    id = 32648; break;   // IDC_NO
    case JZCursorShape::Wait:         id = 32514; break;   // IDC_WAIT
    case JZCursorShape::Arrow:
    default:                          id = 32512; break;   // IDC_ARROW
    }
    return LoadCursorW(nullptr, MAKEINTRESOURCEW(id));
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

JZ_OBJECT_IMPL(JZWindow, JZWidget)

// 未显式设定尺寸的窗口 (构造不再预置 800×600): 首次显示时先按 sizeHint 自适应;
// sizeHint 也给不出 (无布局无子控件, 叶子 sizeHint = 当前尺寸 = 0×0) 再退回该默认,
// 避免开出 0×0 的不可见窗口。
static const int kDefaultWindowW = 800;
static const int kDefaultWindowH = 600;

JZWindow::JZWindow(JZWidget* parent)
    : JZWidget(parent)
{
    m_device = new JZDisplayDevice();
    m_visible = false;

    // 不预置尺寸: 留 0×0, 首次 showEvent 里按 sizeHint 自适应 (见 showEvent)。
    // 最小尺寸由布局 minimumSizeHint 自动推导 (见 JZWidget::minimumWidth/Height),
    // 窗口拖动缩放时不会小于内容所需。

    // 登记到顶层窗口列表 (JZApplication::topLevelWindows / activeWindow,
    // 以及"最后一个窗口关闭则退出"的判定都基于它)
    if (jzApp)
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

// drop target 的注销由 WM_DESTROY -> nativeDestroyed() 完成 (DestroyWindow
// 会同步派发 WM_DESTROY, 那会儿句柄还有效);
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
// 修饰键随事件一起快照 (与键盘事件同源), 供 Ctrl+点击 / Shift 约束等场景直接读取。
static bool jzSendMouseEvent(JZWindow* self, JZMouseEvent::MouseType t, JZMouseEvent::Button b,
                             LPARAM lParam, int wheelDelta = 0)
{
    // globalPos 换算为屏幕逻辑坐标 (Qt globalPos 语义), localPos 由 dispatchMouse 命中后回填
    JZMouseEvent ev(t, b, self->mapToGlobal(jzPhysicalToLogical(lParam, self->device()->scale())),
                    JZPoint(), wheelDelta, jzCurrentKeyModifiers());
    return self->onWindowProcess(&ev);
}

// 应答 WM_IME_REQUEST: TSF 类输入法 (QQ 拼音 / 微软拼音 等) 通过此消息向窗口索取
// "组字窗 / 候选窗"位置。应用不处理时, 部分输入法会把候选窗放到默认位置 —— 表现
// 就是"候选框贴在控件底部、不在输入字符下面"。
// 位置由焦点控件给出 (JZWidget::imeWindowRect, 屏幕逻辑坐标), 这里换算成 IME 要求的
// 物理像素并填进 COMPOSITIONFORM / CANDIDATEFORM。返回 true 表示已填写。
static bool jzHandleImeRequest(JZWindow* self, WPARAM request, LPARAM lpForm)
{
    if (!lpForm || !self->imeActive())
        return false;

    JZWidget* focus = self->focusWidget();
    JZDisplayDevice* dev = self->device();
    if (!focus || !dev)
        return false;

    JZRect lineRect;
    JZPoint candPos;
    if (!focus->imeWindowRect(lineRect, candPos))
        return false;

    const float s = dev->scale();
    auto phys = [s](int v) { return (LONG)std::lround(v * s); };

    switch (request) {
    case IMR_COMPOSITIONWINDOW: {
        COMPOSITIONFORM* cf = (COMPOSITIONFORM*)lpForm;
        cf->dwStyle = CFS_RECT;
        cf->ptCurrentPos.x = phys(lineRect.left());
        cf->ptCurrentPos.y = phys(lineRect.top());
        cf->rcArea.left    = phys(lineRect.left());
        cf->rcArea.top     = phys(lineRect.top());
        cf->rcArea.right   = phys(lineRect.right());
        cf->rcArea.bottom  = phys(lineRect.bottom());
        if (getenv("JZ_IME_DEBUG")) {
            const char* td = getenv("TEMP");
            std::string path = std::string(td ? td : "c:/temp") + "/jz_ime.log";
            std::ofstream f(path, std::ios::app);
            f << "[imeRequest] IMR_COMPOSITIONWINDOW rcArea=(" << cf->rcArea.left
              << "," << cf->rcArea.top << "," << cf->rcArea.right << ","
              << cf->rcArea.bottom << ")\n";
        }
        return true;
    }
    case IMR_CANDIDATEWINDOW: {
        CANDIDATEFORM* cdf = (CANDIDATEFORM*)lpForm;
        cdf->dwStyle = CFS_CANDIDATEPOS;   // 保留 IME 传入的 dwIndex (候选列表序号)
        cdf->ptCurrentPos.x = phys(candPos.x());
        cdf->ptCurrentPos.y = phys(candPos.y());
        if (getenv("JZ_IME_DEBUG")) {
            const char* td = getenv("TEMP");
            std::string path = std::string(td ? td : "c:/temp") + "/jz_ime.log";
            std::ofstream f(path, std::ios::app);
            f << "[imeRequest] IMR_CANDIDATEWINDOW pt=(" << cdf->ptCurrentPos.x
              << "," << cdf->ptCurrentPos.y << ")\n";
        }
        return true;
    }
    // —— 关键: TSF 输入法 (QQ 拼音等) 定位候选窗走的是这条, 不是 IMR_CANDIDATEWINDOW ——
    // 它问"组字串第 dwCharPos 个字符的屏幕位置", 据此摆候选框。不处理就掉到默认位置。
    case IMR_QUERYCHARPOSITION: {
        IMECHARPOSITION* icp = (IMECHARPOSITION*)lpForm;
        if (icp->dwSize < sizeof(IMECHARPOSITION))
            return false;

        JZPoint cpt;
        int lh = 0;
        JZRect doc;
        if (!focus->imeQueryCharPosition(static_cast<int>(icp->dwCharPos), cpt, lh, doc))
            return false;

        icp->pt.x = phys(cpt.x());
        icp->pt.y = phys(cpt.y());
        LONG lhPhys = phys(lh);
        if (lhPhys < 1)
            lhPhys = 1;
        icp->cLineHeight = static_cast<UINT>(lhPhys);
        icp->rcDocument.left   = phys(doc.left());
        icp->rcDocument.top    = phys(doc.top());
        icp->rcDocument.right  = phys(doc.right());
        icp->rcDocument.bottom = phys(doc.bottom());
        if (getenv("JZ_IME_DEBUG")) {
            const char* td = getenv("TEMP");
            std::string path = std::string(td ? td : "c:/temp") + "/jz_ime.log";
            std::ofstream f(path, std::ios::app);
            f << "[imeRequest] IMR_QUERYCHARPOSITION charPos=" << icp->dwCharPos
              << " pt=(" << icp->pt.x << "," << icp->pt.y << ") lineH="
              << icp->cLineHeight << " doc=(" << icp->rcDocument.left << ","
              << icp->rcDocument.top << "," << icp->rcDocument.right << ","
              << icp->rcDocument.bottom << ")\n";
        }
        return true;
    }
    default:
        if (getenv("JZ_IME_DEBUG")) {
            const char* td = getenv("TEMP");
            std::string path = std::string(td ? td : "c:/temp") + "/jz_ime.log";
            std::ofstream f(path, std::ios::app);
            f << "[imeRequest] unhandled request=0x" << std::hex << request
              << std::dec << "\n";
        }
        return false;
    }
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

    // ---- popup 型窗口的"外部按下关闭"不再在 WndProc 里做 ----
    // 以前依赖 SetCapture 抓到客户区外的按下再关窗; 但抓取是全局唯一的, 嵌套弹层
    // (菜单的子菜单)会互相抢走, 抓取窗口之外收不到任何鼠标消息 —— 父菜单的悬停/选择
    // 全部失效。现改为消息泵派发前统一判定, 见 JZWindow::filterPopupPress / pumpOnce。

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
            float s = self->device()->scale();
            self->device()->resize((int)physW, (int)physH);
            // 逻辑尺寸向上取整: 保证 rect()*scale >= 客户区物理尺寸, 根控件
            // 背景 fillRect 覆盖整幅位图, 最外圈像素不会被漏绘成未初始化内存。
            self->windowResized((int)std::ceil(physW / s), (int)std::ceil(physH / s));
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
        // 请求一次"光标离开客户区"通知, 以便 updateHover(nullptr) 给当前悬停控件派发 leave
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        if (jzSendMouseEvent(self, JZMouseEvent::Move, JZMouseEvent::NoButton, lParam))
            return 0;
        break;

    case WM_MOUSELEAVE:
        // 光标离开客户区: 当前悬停控件收 leave (移入其它窗口后下一次 MOVE 会重新 enter)
        self->updateHover(nullptr);
        return 0;

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
        // WM_MOUSEWHEEL 的 lParam 为屏幕物理像素坐标 -> 直接 /scale 即屏幕逻辑坐标
        // (Qt globalPos 语义); 不再 ScreenToClient。
        POINT pt = { (int16_t)LOWORD(lParam), (int16_t)HIWORD(lParam) };
        float s = self->device()->scale();
        JZMouseEvent ev(JZMouseEvent::Wheel, JZMouseEvent::NoButton,
                        JZPoint((int)std::lround(pt.x / s), (int)std::lround(pt.y / s)),
                        JZPoint(), GET_WHEEL_DELTA_WPARAM(wParam) / 120,
                        jzCurrentKeyModifiers());
        if (self->onWindowProcess(&ev))
            return 0;
        break;
    }

    // 光标形状: 客户区内按悬停控件的生效光标 (setCursor/父链继承) 决定;
    // 非客户区 (边框/标题的缩放箭头、resize 手柄等) 交 DefWindowProc。
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            self->applyMouseCursor();
            return TRUE;   // 已显式 SetCursor, 不再走类光标
        }
        break;

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
        // 只有可打印字符才交给控件插入。TranslateMessage 会为控制键也生成 WM_CHAR
        // (退格 0x08 / 回车 0x0D / Esc 0x1B / Del 0x7F ...), 这些都已经由
        // WM_KEYDOWN 处理过; 若在这里再当文本插入, 控制字符会被写进文本里,
        // 表现就是退格/删除"按了没反应"(长度没变, 只是那个字符被替换成控制符)。
        if (wParam < 0x20 || wParam == 0x7F)
            break;
        wchar_t wch = (wchar_t)wParam;
        JZKeyEvent ev(0, jzCurrentKeyModifiers(), jzUtf16ToUtf8(&wch, 1), false);
        if (self->onWindowProcess(&ev))
            return 0;
        break;
    }

    // ---- 输入法 (IME) ----

    // TSF 类输入法 (QQ 拼音 / 微软拼音 等) 通过 WM_IME_REQUEST 向应用索取组字窗/
    // 候选窗位置; 不处理则候选框会退化到默认位置 (控件底部)。位置由焦点控件给出,
    // 见 JZWidget::imeWindowRect。未处理的请求 (如 IMR_QUERYCHARPOSITION) 交回默认处理。
    case WM_IME_REQUEST:
        if (jzHandleImeRequest(self, wParam, lParam))
            return TRUE;
        break;

    // 不让系统画组字窗: 组字串由控件在 paintEvent 里自绘, 候选窗仍交给 IME,
    // 位置由 CANDIDATEFORM 指定。必须 break 继续走 DefWindowProc (不能 return 0),
    // 否则 IME 收不到窗口上下文, 候选窗/状态窗不会正确建立。
    //
    // 焦点不接受输入法时 (imeActive() == false) 连候选窗/状态窗一起隐藏:
    // 这是"断开 IMC"的替代做法 —— IMC 保持关联, 中英状态不被重置。
    case WM_IME_SETCONTEXT:
        lParam &= ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW);
        if (!self->imeActive())
            lParam &= ~static_cast<LPARAM>(ISC_SHOWUIALL);
        break;

    case WM_IME_STARTCOMPOSITION: {
        if (!self->imeActive()) {
            self->cancelImeComposition();   // 焦点不支持输入: 组字立即取消, 不上屏
            return 0;
        }
        self->updateImeUi(true);            // 兜底: 确保候选窗 UI 已打开
        self->setImeGotResult(false);
        JZImeEvent ev(JZImeEvent::StartComposition);
        self->onWindowProcess(&ev);
        return 0;
    }

    case WM_IME_ENDCOMPOSITION: {
        self->setImeGotResult(false);
        JZImeEvent ev(JZImeEvent::EndComposition);
        self->onWindowProcess(&ev);
        return 0;
    }

    case WM_IME_NOTIFY:
        // 只有候选窗相关消息自己消化; 其余 (IMN_SETOPENSTATUS /
        // IMN_SETCONVERSIONMODE / IMN_GUIDELINE 等) 必须下传 DefWindowProc,
        // 否则部分输入法 (搜狗/五笔等) 的中英状态、状态窗不刷新。
        if (wParam == IMN_OPENCANDIDATE || wParam == IMN_CHANGECANDIDATE ||
            wParam == IMN_CLOSECANDIDATE) {
            if (!self->imeActive())
                return 0;                   // 不接受输入: 不定位候选窗
            JZImeEvent ev(JZImeEvent::Candidate);
            self->onWindowProcess(&ev);
            return 0;
        }
        break;

    case WM_IME_COMPOSITION: {
        // 焦点不接受输入法: 组字内容一律不派发 (也不上屏)
        if (!self->imeActive()) {
            self->cancelImeComposition();
            return 0;
        }
        // 标志严格按 lParam 置位: 构造时不能预先带 CompStr, 否则只有
        // GCS_CURSORPOS 时控件会拿空组字串覆盖掉正在显示的组字内容。
        JZImeEvent ev(0);
        HIMC hImc = ImmGetContext(hwnd);
        if (hImc) {
            // 组字串 (未上屏)。lParam == 0 表示组字内容被清空 (退格删完等),
            // 同样要通知控件, 否则残留组字串不会消失。
            std::wstring wcomp;
            if ((lParam & GCS_COMPSTR) || lParam == 0) {
                LONG len = ImmGetCompositionStringW(hImc, GCS_COMPSTR, nullptr, 0);
                if (len > 0) {
                    wcomp.resize((size_t)(len / (int)sizeof(wchar_t)));
                    ImmGetCompositionStringW(hImc, GCS_COMPSTR, &wcomp[0], len);
                }
                ev.setCompString(jzUtf16ToUtf8(wcomp.c_str(), (int)wcomp.size()));
                ev.setFlag(JZImeEvent::CompStr);

                // 每字符属性 (下标按 UTF-16): 必须与组字串同批读取,
                // 单独 GCS_COMPATTR 到来时属性会与已缩短的组字串错位。
                LONG alen = ImmGetCompositionStringW(hImc, GCS_COMPATTR, nullptr, 0);
                std::vector<unsigned char> attr;
                if (alen > 0) {
                    attr.assign((size_t)alen, 0);
                    ImmGetCompositionStringW(hImc, GCS_COMPATTR, attr.data(), alen);
                }
                ev.setCompAttr(attr);

                // 组字光标位置 (UTF-16 下标): 无条件读。微软拼音逐字母输入时
                // lParam 往往只有 GCS_COMPSTR|GCS_COMPATTR、不带 GCS_CURSORPOS 位,
                // 只按位读会一直拿不到, 插入点光标就永远停在组字串最前面。
                //
                // 只能用**返回值形式**: ImmGetCompositionString 对 GCS_CURSORPOS
                // 不写缓冲区、直接把位置当返回值, 所以"缓冲区形式"会返回 >0 让人
                // 误判成功, 而 buf 始终是初值 0 —— 正是"光标不跟到末尾"的根因。
                //
                // lParam 未带 GCS_CURSORPOS 位时读到 0 视为失败, 落到组字串末尾:
                // 打字过程中光标本来就该跟着组字串走; 真要把光标移到组字串开头,
                // IME 会置位显式下发, 由下面那个分支按 0 处理。
                LONG pos = ImmGetCompositionStringW(hImc, GCS_CURSORPOS, nullptr, 0);
                if (pos < 0 || (pos == 0 && !(lParam & GCS_CURSORPOS)))
                    pos = (LONG)wcomp.size();
                if (pos > (LONG)wcomp.size())
                    pos = (LONG)wcomp.size();
                ev.setCompCursor((int)pos);
                ev.setFlag(JZImeEvent::CursorPos);
            }
            // 光标单独变化 (组字串未变, 如组字串内按方向键 / Home)
            if (lParam & GCS_CURSORPOS) {
                // 同上用返回值形式; 位已置起说明 IME 明确下发了位置, 0 也当真
                LONG pos = ImmGetCompositionStringW(hImc, GCS_CURSORPOS, nullptr, 0);
                if (pos >= 0) {
                    ev.setCompCursor((int)pos);
                    ev.setFlag(JZImeEvent::CursorPos);
                }
            }
            // 结果串 (上屏)
            if (lParam & GCS_RESULTSTR) {
                LONG rlen = ImmGetCompositionStringW(hImc, GCS_RESULTSTR, nullptr, 0);
                std::string res;
                if (rlen > 0) {
                    std::wstring wres((size_t)(rlen / (int)sizeof(wchar_t)), 0);
                    ImmGetCompositionStringW(hImc, GCS_RESULTSTR, &wres[0], rlen);
                    res = jzUtf16ToUtf8(wres.c_str(), (int)wres.size());
                }
                ev.setResultString(res);
                ev.setFlag(JZImeEvent::ResultStr);
            }
            ImmReleaseContext(hwnd, hImc);
        }
        if (ev.flags() != 0) {
            if (ev.hasFlag(JZImeEvent::ResultStr) && !ev.resultString().empty())
                self->setImeGotResult(true);
            self->onWindowProcess(&ev);
        }
        return 0;   // 关键: 不转发 DefWindowProc, 否则它会再补一次 WM_CHAR 重复上屏
    }

    // 兼容老式输入法: 结果串若已由 GCS_RESULTSTR 上屏, 这里不能再插一次
    case WM_IME_CHAR: {
        if (!self->imeGotResult() && wParam >= 0x20 && wParam != 0x7F) {
            wchar_t wch = (wchar_t)wParam;
            JZKeyEvent ev(0, jzCurrentKeyModifiers(), jzUtf16ToUtf8(&wch, 1), false);
            self->onWindowProcess(&ev);
        }
        return 0;
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
        self->close();          // Qt 语义: WM_CLOSE -> close() -> sendEvent(closeEvent)
        return 0;

    case WM_DESTROY:
        self->nativeDestroyed();
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
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

// ==================== 拖放 (OLE drop target) ====================
// 每个窗口把自己登记成一个 IDropTarget, 于是资源管理器/浏览器等外部数据源可以把
// 文件、图片、文本拖进来。OLE 的四个回调在这里翻译成 JZDrag*Event, 交给 JZWindow 的
// dragEnter/dragOver/dragDrop 按控件树分发 (命中 + 父链上抛, 同 Qt)。
//
// 选 IDropTarget 而不是 WM_DROPFILES: 后者只认文件列表, 浏览器直接拖图片
// (CF_DIB/CF_HBITMAP)、跨应用的自定义格式一律拿不到; RegisterDragDrop 才是 Qt 底层
// 走的那条路 (Qt 的 QWindowsDropTarget 同样如此)。

#include <oleidl.h>
#include <shellapi.h>      // DragQueryFileW (CF_HDROP)

// 屏幕物理像素 (OLE 的 POINTL) -> 屏幕逻辑像素 (与 JZMouseEvent::globalPos 同一坐标系)
static JZPoint jzDragGlobalLogical(JZWindow* self, POINTL pt)
{
    float s = self->device() ? self->device()->scale() : 1.0f;
    return JZPoint((int)std::lround(pt.x / s), (int)std::lround(pt.y / s));
}

static uint32_t jzDragModifiers(DWORD keyState)
{
    uint32_t mods = JZKeyEvent::NoModifier;
    if (keyState & MK_SHIFT)   mods |= JZKeyEvent::ShiftModifier;
    if (keyState & MK_CONTROL) mods |= JZKeyEvent::CtrlModifier;
    if (keyState & MK_ALT)     mods |= JZKeyEvent::AltModifier;
    return mods;
}

// OLE effect 位 -> 本框架的动作位 (两者顺序不同, 逐个翻译)
static JZDropActions jzDragActions(DWORD effect)
{
    JZDropActions actions = JZDropActionNone;
    if (effect & DROPEFFECT_COPY) actions |= JZDropActionCopy;
    if (effect & DROPEFFECT_MOVE) actions |= JZDropActionMove;
    if (effect & DROPEFFECT_LINK) actions |= JZDropActionLink;
    return actions;
}

// 系统建议的动作 (Qt 同款规则): Ctrl=Copy / Shift=Move / Alt=Link,
// 什么都不按则优先 Move —— 拖文件时 OLE 通常同时给 Copy|Move, 而"挪过来"更合直觉。
// 想要的动作拖方不支持时, 回落到它支持的第一个。
static JZDropAction jzDragProposed(DWORD keyState, JZDropActions possible)
{
    const uint32_t mods = jzDragModifiers(keyState);
    const bool ctrl  = (mods & JZKeyEvent::CtrlModifier)  != 0;
    const bool shift = (mods & JZKeyEvent::ShiftModifier) != 0;
    const bool alt   = (mods & JZKeyEvent::AltModifier)   != 0;

    JZDropAction want = JZDropActionIgnore;
    if (ctrl && shift)                      want = JZDropActionLink;
    else if (ctrl)                          want = JZDropActionCopy;
    else if (shift)                         want = JZDropActionMove;
    else if (alt)                           want = JZDropActionLink;
    else if (possible & JZDropActionMove)   want = JZDropActionMove;
    else if (possible & JZDropActionCopy)   want = JZDropActionCopy;
    else if (possible & JZDropActionLink)   want = JZDropActionLink;

    if ((possible & want) == 0) {
        if (possible & JZDropActionCopy)         want = JZDropActionCopy;
        else if (possible & JZDropActionMove)    want = JZDropActionMove;
        else if (possible & JZDropActionLink)    want = JZDropActionLink;
        else                                     want = JZDropActionNone;
    }
    return want;
}

static DWORD jzDragEffect(JZDropAction action)
{
    switch (action) {
    case JZDropActionCopy: return DROPEFFECT_COPY;
    case JZDropActionMove: return DROPEFFECT_MOVE;
    case JZDropActionLink: return DROPEFFECT_LINK;
    default:               return DROPEFFECT_NONE;
    }
}

static FORMATETC jzFormatEtc(CLIPFORMAT cf)
{
    FORMATETC fe;
    memset(&fe, 0, sizeof(fe));
    fe.cfFormat = (WORD)cf;
    fe.dwAspect = DVASPECT_CONTENT;
    fe.lindex = -1;
    fe.tymed = TYMED_HGLOBAL;
    return fe;
}

// 数据源里有没有这个格式 (GetData 之前先问一圈, 免得挨个试)
static bool jzDataHasFormat(IDataObject* data, CLIPFORMAT cf, DWORD tymed = TYMED_HGLOBAL)
{
    if (!data)
        return false;
    FORMATETC fe = jzFormatEtc(cf);
    fe.tymed = tymed;
    return SUCCEEDED(data->QueryGetData(&fe));
}

// 取一份 HGLOBAL 载荷的副本 (失败返回空 JZByteArray)。
// 注意: CF_HDROP 的 HGLOBAL 直接就是 DROPFILES*, 而 DragQueryFile 的偏移量是相对它的,
// 于是这份**副本**也能当 HDROP 用 (下面 jzMimeDataFrom 就是这么干的)。
static JZByteArray jzGetGlobalData(IDataObject* data, CLIPFORMAT cf)
{
    if (!data)
        return JZByteArray();

    FORMATETC fe = jzFormatEtc(cf);
    STGMEDIUM stg;
    memset(&stg, 0, sizeof(stg));
    if (FAILED(data->GetData(&fe, &stg)))
        return JZByteArray();
    if (stg.tymed != TYMED_HGLOBAL || !stg.hGlobal) {
        ReleaseStgMedium(&stg);
        return JZByteArray();
    }

    void* mem = GlobalLock(stg.hGlobal);
    SIZE_T bytes = GlobalSize(stg.hGlobal);
    JZByteArray out;
    if (mem && bytes > 0)
        out = JZByteArray((const char*)mem, (int)bytes);
    if (mem)
        GlobalUnlock(stg.hGlobal);
    ReleaseStgMedium(&stg);
    return out;
}

// CF_DIB -> RGBA 图像。DIB 的行自下而上存放 (biHeight > 0 时), 每行按 4 字节对齐。
static JZImage jzImageFromDib(const JZByteArray& dib)
{
    BITMAPINFOHEADER bi;
    if (dib.size() < (int)sizeof(bi))
        return JZImage();
    memcpy(&bi, dib.constData(), sizeof(bi));
    if (bi.biWidth <= 0 || bi.biHeight == 0)
        return JZImage();

    const int w = bi.biWidth;
    const int h = bi.biHeight < 0 ? -bi.biHeight : bi.biHeight;
    const bool bottomUp = bi.biHeight > 0;
    const int bpp = bi.biBitCount;

    // 像素数据在头之后: <=8bpp 跟调色板, BI_BITFIELDS 跟三个通道掩码
    size_t offset = sizeof(bi);
    if (bpp <= 8)
        offset += (size_t)(bi.biClrUsed ? bi.biClrUsed : (1u << bpp)) * sizeof(RGBQUAD);
    else if (bi.biCompression == BI_BITFIELDS)
        offset += 3 * sizeof(DWORD);
    if (offset > (size_t)dib.size())
        return JZImage();

    const size_t stride = ((size_t)w * bpp + 31) / 32 * 4;   // 源行字节数 (4 字节对齐)
    if (offset + stride * (size_t)h > (size_t)dib.size())
        return JZImage();

    const uint8_t* bits = (const uint8_t*)dib.constData() + offset;
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) {
        // 目标第 y 行 (自上而下) 对应源的哪一行
        const size_t srcRow = bottomUp ? (size_t)(h - 1 - y) : (size_t)y;
        const uint8_t* src = bits + srcRow * stride;
        uint8_t* dst = rgba.data() + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            uint8_t r = 0, g = 0, b = 0, a = 255;
            switch (bpp) {
            case 32:
                b = src[x * 4 + 0]; g = src[x * 4 + 1]; r = src[x * 4 + 2]; a = src[x * 4 + 3];
                // 多数拖图的 DIB 把 alpha 留 0, 照它画出来是全透明的图
                if (a == 0) a = 255;
                break;
            case 24:
                b = src[x * 3 + 0]; g = src[x * 3 + 1]; r = src[x * 3 + 2];
                break;
            case 8: {
                const RGBQUAD* pal = (const RGBQUAD*)((const uint8_t*)dib.constData() + sizeof(bi));
                const RGBQUAD& c = pal[src[x]];
                b = c.rgbBlue; g = c.rgbGreen; r = c.rgbRed;
                break;
            }
            default:
                return JZImage();   // 更低位深 (4/1bpp) 的 DIB 不支持
            }
            dst[x * 4 + 0] = r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = b;
            dst[x * 4 + 3] = a;
        }
    }
    return JZImage::fromRGBA(rgba.data(), w, h);
}

// CF_HBITMAP -> RGBA 图像 (GetDIBits 拉成 32bpp; 得到的行同样自下而上)
static JZImage jzImageFromHBitmap(HBITMAP hbmp)
{
    BITMAP bm;
    memset(&bm, 0, sizeof(bm));
    if (!GetObjectW(hbmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight == 0)
        return JZImage();

    const int w = bm.bmWidth;
    const int h = bm.bmHeight;
    std::vector<uint8_t> bits((size_t)w * h * 4);

    BITMAPINFOHEADER bi;
    memset(&bi, 0, sizeof(bi));
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = h;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    HDC hdc = GetDC(nullptr);
    int rows = 0;
    if (hdc) {
        rows = GetDIBits(hdc, hbmp, 0, h, bits.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, hdc);
    }
    if (rows != h)
        return JZImage();

    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = bits.data() + (size_t)(h - 1 - y) * w * 4;
        uint8_t* dst = rgba.data() + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3] ? src[x * 4 + 3] : (uint8_t)255;
        }
    }
    return JZImage::fromRGBA(rgba.data(), w, h);
}

// CF_BITMAP (HBITMAP 句柄) -> RGBA 图像。
// 这种格式的句柄要么由 STGMEDIUM::hBitmap 直接给出 (TYMED_GDI), 要么装在 HGLOBAL 里,
// 两种都得认 —— 只按 TYMED_HGLOBAL 取的多数数据源会取不到。
static JZImage jzImageFromBitmapFormat(IDataObject* data, CLIPFORMAT cf)
{
    if (!data)
        return JZImage();

    FORMATETC fe = jzFormatEtc(cf);
    fe.tymed = TYMED_HGLOBAL | TYMED_GDI;
    if (FAILED(data->QueryGetData(&fe)))
        return JZImage();

    STGMEDIUM stg;
    memset(&stg, 0, sizeof(stg));
    if (FAILED(data->GetData(&fe, &stg)))
        return JZImage();

    HBITMAP hbmp = nullptr;
    void* locked = nullptr;
    if (stg.tymed == TYMED_GDI) {
        hbmp = stg.hBitmap;
    } else if (stg.tymed == TYMED_HGLOBAL && stg.hGlobal) {
        locked = GlobalLock(stg.hGlobal);
        if (locked && GlobalSize(stg.hGlobal) >= sizeof(HBITMAP))
            hbmp = *(HBITMAP*)locked;
    }

    JZImage img;
    if (hbmp)
        img = jzImageFromHBitmap(hbmp);
    if (locked)
        GlobalUnlock(stg.hGlobal);
    ReleaseStgMedium(&stg);
    return img;
}

// IDataObject -> JZMimeData: 先吸收成 urls / text / image 三个便捷项,
// 再把它声明的其余格式按 MIME 名原样收进通用项 (自定义格式能自取自存)。
static std::shared_ptr<JZMimeData> jzMimeDataFrom(IDataObject* data)
{
    auto mime = std::make_shared<JZMimeData>();
    if (!data)
        return mime;

    // ---- 文件列表 (资源管理器拖文件): CF_HDROP ----
    if (jzDataHasFormat(data, CF_HDROP)) {
        JZByteArray raw = jzGetGlobalData(data, CF_HDROP);
        if (!raw.isEmpty()) {
            HDROP hdrop = (HDROP)raw.data();
            UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::string> paths;
            for (UINT i = 0; i < count; ++i) {
                UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
                std::wstring ws((size_t)len, L'\0');
                if (DragQueryFileW(hdrop, i, &ws[0], len + 1) == 0)
                    continue;
                paths.push_back(jzUtf16ToUtf8(ws.c_str(), (int)len));
            }
            mime->setUrls(paths);
        }
    }

    // ---- 文本 ----
    if (jzDataHasFormat(data, CF_UNICODETEXT)) {
        JZByteArray raw = jzGetGlobalData(data, CF_UNICODETEXT);
        if (!raw.isEmpty()) {
            std::string utf8 = jzUtf16ToUtf8((const wchar_t*)raw.constData(),
                                             raw.size() / (int)sizeof(wchar_t));
            if (!utf8.empty())
                mime->setText(utf8);
        }
    } else if (jzDataHasFormat(data, CF_TEXT)) {
        // 老程序的 ANSI 文本 (CP_ACP -> UTF-16 -> UTF-8)
        JZByteArray raw = jzGetGlobalData(data, CF_TEXT);
        if (!raw.isEmpty()) {
            int wide = MultiByteToWideChar(CP_ACP, 0, raw.constData(), raw.size(), nullptr, 0);
            std::wstring ws((size_t)(wide > 0 ? wide : 0), L'\0');
            if (wide > 0)
                MultiByteToWideChar(CP_ACP, 0, raw.constData(), raw.size(), &ws[0], wide);
            mime->setText(jzUtf16ToUtf8(ws.c_str(), wide));
        }
    }

    // ---- 图像: CF_DIB 优先 (浏览器/看图器拖图多半给这个) ----
    bool gotImage = false;
    if (jzDataHasFormat(data, CF_DIB)) {
        JZImage img = jzImageFromDib(jzGetGlobalData(data, CF_DIB));
        if (!img.isNull()) {
            mime->setImage(img);
            gotImage = true;
        }
    }
    if (!gotImage && jzDataHasFormat(data, CF_BITMAP, TYMED_HGLOBAL | TYMED_GDI)) {
        JZImage img = jzImageFromBitmapFormat(data, CF_BITMAP);
        if (!img.isNull()) {
            mime->setImage(img);
            gotImage = true;
        }
    }

    // ---- 其余格式按名字收进通用项 ----
    IEnumFORMATETC* e = nullptr;
    if (SUCCEEDED(data->EnumFormatEtc(DATADIR_GET, &e)) && e) {
        FORMATETC cur;
        memset(&cur, 0, sizeof(cur));
        while (e->Next(1, &cur, nullptr) == S_OK) {
            // 上面已转成便捷项的、以及没法当字节用的格式跳过
            const bool handled = cur.cfFormat == CF_HDROP || cur.cfFormat == CF_UNICODETEXT ||
                                 cur.cfFormat == CF_TEXT || cur.cfFormat == CF_DIB ||
                                 cur.cfFormat == CF_DIBV5 || cur.cfFormat == CF_BITMAP ||
                                 cur.cfFormat == CF_PALETTE ||
                                 cur.cfFormat == CF_METAFILEPICT || cur.cfFormat == CF_ENHMETAFILE ||
                                 cur.cfFormat == CF_LOCALE || cur.cfFormat == CF_PENDATA;
            if (!handled && (cur.tymed & TYMED_HGLOBAL)) {
                wchar_t nameBuf[256] = { 0 };
                std::string name;
                if (GetClipboardFormatNameW(cur.cfFormat, nameBuf, 256) > 0)
                    name = jzUtf16ToUtf8(nameBuf, (int)wcslen(nameBuf));
                else
                    name = "cf/" + std::to_string((int)cur.cfFormat);

                JZByteArray raw = jzGetGlobalData(data, (CLIPFORMAT)cur.cfFormat);
                if (!name.empty() && !raw.isEmpty())
                    mime->setData(name, raw);
            }
            if (cur.ptd)
                CoTaskMemFree(cur.ptd);
            memset(&cur, 0, sizeof(cur));
        }
        e->Release();
    }
    return mime;
}

// 一个窗口一份: 接到 OLE 回调后转交给 JZWindow (那个才是控件树的分发者)。
class JZWindowDropTarget : public IDropTarget
{
public:
    explicit JZWindowDropTarget(JZWindow* win) : m_win(win), m_ref(1) {}
    JZWindowDropTarget(const JZWindowDropTarget&) = delete;
    JZWindowDropTarget& operator=(const JZWindowDropTarget&) = delete;

    // ---- IUnknown ----
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override
    {
        if (!ppv)
            return E_INVALIDARG;
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0)
            delete this;
        return (ULONG)r;
    }

    // ---- IDropTarget ----
    STDMETHODIMP DragEnter(IDataObject* data, DWORD keyState, POINTL pt, DWORD* effect) override
    {
        if (!effect)
            return E_INVALIDARG;

        const JZDropActions actions = jzDragActions(*effect);
        const JZDropAction proposed = jzDragProposed(keyState, actions);
        if (!data || actions == JZDropActionNone) {
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }

        bindDataObject(data);
        const bool ok = m_win->dragEnter(m_mime, actions, proposed, jzDragModifiers(keyState),
                                         jzDragGlobalLogical(m_win, pt));
        *effect = ok ? jzDragEffect(m_win->dropAction()) : DROPEFFECT_NONE;
        return S_OK;
    }

    STDMETHODIMP DragOver(DWORD keyState, POINTL pt, DWORD* effect) override
    {
        if (!effect)
            return E_INVALIDARG;

        const JZDropActions actions = jzDragActions(*effect);
        if (actions == JZDropActionNone) {
            m_win->dragLeave();
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }

        const JZDropAction proposed = jzDragProposed(keyState, actions);
        const bool ok = m_win->dragOver(actions, proposed, jzDragModifiers(keyState),
                                        jzDragGlobalLogical(m_win, pt));
        *effect = ok ? jzDragEffect(m_win->dropAction()) : DROPEFFECT_NONE;
        return S_OK;
    }

    STDMETHODIMP DragLeave() override
    {
        m_win->dragLeave();
        return S_OK;
    }

    STDMETHODIMP Drop(IDataObject* data, DWORD keyState, POINTL pt, DWORD* effect) override
    {
        if (!effect)
            return E_INVALIDARG;

        const JZDropActions actions = jzDragActions(*effect);
        const JZDropAction proposed = jzDragProposed(keyState, actions);
        if (data && data != m_dataObject)
            bindDataObject(data);   // 个别数据源只在真正放下时才给数据对象
        if (!m_mime)
            m_mime = jzMimeDataFrom(m_dataObject);

        const bool ok = m_win->dragDrop(actions, proposed, jzDragModifiers(keyState),
                                        jzDragGlobalLogical(m_win, pt));
        *effect = ok ? jzDragEffect(m_win->dropAction()) : DROPEFFECT_NONE;

        releaseDataObject();   // 一次拖放结束: 数据源与中间态全部释放 (下一轮 DragEnter 重建)
        return S_OK;
    }

private:
    ~JZWindowDropTarget()
    {
        releaseDataObject();
    }

    // 整个拖放期间持有数据源: 延迟渲染 / 流格式的载荷要等真正放下那一刻才取,
    // 松开早了会崩。
    void bindDataObject(IDataObject* data)
    {
        if (m_dataObject == data)
            return;
        releaseDataObject();
        m_dataObject = data;
        if (m_dataObject)
            m_dataObject->AddRef();
        m_mime = jzMimeDataFrom(m_dataObject);
    }

    void releaseDataObject()
    {
        if (m_dataObject) {
            m_dataObject->Release();
            m_dataObject = nullptr;
        }
        m_mime.reset();
    }

    JZWindow*                    m_win = nullptr;
    LONG                         m_ref = 1;
    IDataObject*                 m_dataObject = nullptr;
    std::shared_ptr<JZMimeData>  m_mime;
};

// COM/OLE 初始化: 每进程一次即可 ( RegisterDragDrop 之前必须 OleInitialize )。
static bool jzEnsureOle()
{
    static bool tried = false;
    static bool ok = false;
    if (!tried) {
        tried = true;
        ok = SUCCEEDED(OleInitialize(nullptr));
    }
    return ok;
}

static void registerWindowClass()
{
    static bool registered = false;
    if (registered)
        return;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    // 不加 CS_DROPSHADOW: 该样式会让系统为每个窗口额外创建一个 "SysShadow"
    // 影子窗口, 它比窗口矩形**向右、向下各多出几像素**(100% DPI 2px, 200% DPI 5px),
    // 且锚点是窗口的左上角 —— 表现为所有窗口的右边与下边多出一条硬边灰带, 看上去
    // 像凭空多了一圈"边框"(窗口本体正常, 灰带在窗口矩形之外)。现代 Windows 里
    // DWM 本就会给带框窗口(WS_CAPTION/WS_THICKFRAME)绘制投影, 类上再挂旧式投影
    // 只会在 DWM 投影之外叠一条硬边, 故这里去掉。
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
    m_device->render(this, rc);
}

// 惰性创建: 原生窗口 + 渲染设备初始化 (首次 showEvent 触发, 幂等)。
// 尺寸/标题取当前值 (构造后可能被 resize/setTitle 调整过)。
void JZWindow::createWindow()
{
    if (m_windowCreated)
        return;
    m_windowCreated = true;

    int w = width(), h = height();
    // 初次设定 DPI 缩放: 已显式设定 (popup 跟随 owner 显示器) 则保留;
    // 否则优先取 owner 所在显示器 DPI, 没有 owner 再退主屏 DPI (多屏/HiDPI 下
    // 弹层与 owner 才在同一坐标系)。
    if (!m_scaleExplicit) {
        HWND owner = nullptr;
        if (JZWidget* p = parentWidget()) {
            if (JZWindow* pw = p->window())
                owner = (HWND)pw->nativeHandle();
        }
        m_device->setScale(JZScreen::logicalDpiFor(owner) / 96.0f);
    }

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

    // 初始位置 (等价 Qt: create_sys 用 crect 初始化, 出现即就位无跳变):
    //   1. 显式 moveScreen 记录的屏幕逻辑坐标 (WA_Moved 等价) —— 优先;
    //   2. Modal/Popup 自动定位: 相对 owner 窗口居中;
    //   3. 其余 (Normal 主窗口等) CW_USEDEFAULT 交给 Windows 级联。
    int posX = CW_USEDEFAULT, posY = CW_USEDEFAULT;
    float s = m_device->scale();
    if (m_hasScreenPos) {
        posX = (int)std::lround(m_screenX * s);
        posY = (int)std::lround(m_screenY * s);
    } else if (m_autoPosition && m_windowType != Normal && owner) {
        RECT orc;
        if (GetWindowRect(owner, &orc)) {
            int physW = rc.right - rc.left, physH = rc.bottom - rc.top;
            posX = orc.left + ((orc.right - orc.left) - physW) / 2;
            posY = orc.top + ((orc.bottom - orc.top) - physH) / 2;
        }
    }

    std::wstring wtitle = jzUtf8ToUtf16(m_title);
    m_hwnd = CreateWindowExW(exStyle, L"JZWindowWndClass", wtitle.c_str(), style,
                             posX, posY,
                             rc.right - rc.left, rc.bottom - rc.top,
                             owner, nullptr, GetModuleHandleW(nullptr), this);
    if (m_hwnd) {
        unsigned physW = (unsigned)std::lround(w * m_device->scale());
        unsigned physH = (unsigned)std::lround(h * m_device->scale());
        m_device->init(m_hwnd, physW, physH);
        if (m_top)
            SetWindowPos((HWND)m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        // 登记成拖放目标 (外部拖文件/图片进来)。OLE 初始化一次即可 (进程级)。
        // 失败不影响窗口本身 —— 只是不能接收拖放。
        if (jzEnsureOle()) {
            JZWindowDropTarget* dt = new JZWindowDropTarget(this);
            if (SUCCEEDED(RegisterDragDrop((HWND)m_hwnd, dt)))
                m_dropTarget = dt;
            else
                dt->Release();
        }

        // 立即缓存默认输入上下文 (IMC)。关键: 之后若需要断开 (ImmAssociateContext(NULL)),
        // ImmGetContext 会返回 NULL, 默认 IMC 句柄再也拿不到 —— 整个窗口的输入法就废了。
        // 参考实现 (test_ime WM_CREATE) 也是创建时立刻缓存。聚焦/失焦切换时复用此句柄。
        if (!m_imeContext) {
            HIMC hImc = ImmGetContext((HWND)m_hwnd);
            if (hImc) {
                m_imeContext = hImc;
                ImmReleaseContext((HWND)m_hwnd, hImc);
            }
        }
    }
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
    if (m_hwnd) {
        ShowWindow((HWND)m_hwnd, SW_SHOW);
        // Popup 不再 SetCapture: 抓取是全局唯一的, 嵌套弹层(子菜单)会抢走抓取,
        // 父弹层从此收不到鼠标消息(悬停/选择失效)。"外部按下关闭"由消息泵在
        // 派发按下消息前统一判定, 见 JZWindow::filterPopupPress。
    }
}

void JZWindow::hideEvent(JZHideEvent* event)
{
    JZWidget::hideEvent(event);

    if (m_windowCreated && m_hwnd) {
        // 窗口隐藏: 任何进行中的鼠标抓取都失效 (控件可能已被移除/不可见)
        if (m_mouseGrabber)
            releaseMouse(nullptr);
        // 悬停态随窗口一起失效, 下次显示后由 mouseMove 重新建立
        m_hoverWidget = nullptr;
        // 只有抓取确实在本窗口手里时才释放, 避免误放别的窗口(或子弹层)的抓取
        if (GetCapture() == (HWND)m_hwnd)
            ReleaseCapture();
        ShowWindow((HWND)m_hwnd, SW_HIDE);
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
    if (m_hwnd)
        SetCapture((HWND)m_hwnd); // 含客户区外的 move/up 都路由到本窗口
}

void JZWindow::releaseMouse(JZWidget* w)
{
    // w==nullptr: 强制解除; 否则仅当 w 是当前 grabber 时才解除 (避免误伤别人的抓取)
    if (w && w != m_mouseGrabber)
        return;
    m_mouseGrabber = nullptr;
    if (m_hwnd && (GetCapture() == (HWND)m_hwnd))
        ReleaseCapture();
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

    POINT pt = { (LONG)screenX, (LONG)screenY };
    auto contains = [pt](JZWindow* w) {
        RECT rc;
        return GetWindowRect((HWND)w->nativeHandle(), &rc) && PtInRect(&rc, pt) ? true : false;
    };

    bool insideAny = false;
    for (JZWindow* w : popups)
    {
        if (contains(w))
        {
            insideAny = true;
            break;
        }
    }

    // 关闭所有不含按下点的弹层: 子菜单开着时点父菜单 -> 只关子菜单, 父菜单照常响应
    bool closed = false;
    for (JZWindow* w : popups)
    {
        if (!contains(w))
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
    if (gate && m_hwnd && m_focusWidget && m_focusWidget->imeEnabled())
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
// (自身 setCursor 或沿父链继承, 全无则箭头)。目标 = 拖拽抓取中的 grabber,
// 否则为光标下的最深控件 (命中为空以窗口自身兜底)。
// 被 WM_SETCURSOR (客户区内) 调用; setCursor / 鼠标派发后需要即时刷新时也会调。
void JZWindow::applyMouseCursor()
{
    if (!m_hwnd || !m_device)
        return;

    // 光标物理屏幕坐标 -> 逻辑坐标 (与 WndProc 鼠标消息同一换算)
    POINT pt;
    if (!GetCursorPos(&pt))
        return;
    // 只在光标位于本窗口客户区内才接管 (拖拽出窗后交给系统/其它窗口)
    POINT cl = pt;
    ScreenToClient((HWND)m_hwnd, &cl);
    RECT rc;
    GetClientRect((HWND)m_hwnd, &rc);
    if (cl.x < 0 || cl.y < 0 || cl.x >= rc.right || cl.y >= rc.bottom)
        return;

    float s = m_device->scale();
    JZPoint g((int)std::lround(pt.x / s), (int)std::lround(pt.y / s));

    // 拖拽抓取中: 光标以 grabber 为准 (拖动过程不因光标移出控件命中带变回箭头)
    JZWidget* target = m_mouseGrabber ? m_mouseGrabber : hitTest(g);
    if (!target)
        target = this;

    // 全屏蒙版期间: 蒙版下方一律用窗口默认箭头, 不露出下层控件的光标
    // (如滑块的大小光标); 蒙版自身子控件 (取消按钮) 仍按其生效光标显示。
    if (m_inputGate && m_inputGate->isVisible() && target != m_inputGate
        && !jzWindowHitInGate(target, m_inputGate))
        target = this;

    SetCursor(jzCursorHandle(target->cursor().shape()));
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
    if (changed && m_hwnd) {
        DWORD style = (DWORD)GetWindowLongPtrW((HWND)m_hwnd, GWL_STYLE);
        RECT rc = jzWindowRectForClient((HWND)m_hwnd, style, w, h, m_device->scale());
        SetWindowPos((HWND)m_hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);   // WM_SIZE -> windowResized
        update();
    }
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

// 显示器 DPI 变化 (WM_DPICHANGED): 更新设备缩放 (重建字体)
void JZWindow::dpiChanged()
{
    m_device->setScale(JZScreen::logicalDpiFor(m_hwnd) / 96.0f);
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
        // 无焦点控件时返回 false, 交回 DefWindowProc —— 不要静默吞掉,
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
        return true;   // 无人消费也吞掉, 不外泄到 DefWindowProc
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
// 直到被接受或整条控件链都不要 -> 返回 false, 由 WndProc 转交 DefWindowProc
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

// ==================== 拖放 ====================

// 注销 OLE drop target。必须在 DestroyWindow **之前**: 窗口句柄一旦销毁,
// RevokeDragDrop 就找不到它了 (这也解释了为什么 RegisterDragDrop 放在 createWindow 里)。
void JZWindow::revokeDropTarget()
{
    if (m_dropTarget) {
        if (m_hwnd)
            RevokeDragDrop((HWND)m_hwnd);
        ((JZWindowDropTarget*)m_dropTarget)->Release();
        m_dropTarget = nullptr;
    }
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
    JZDropEvent ev(JZEvent_drop, JZPoint(globalLogical.x() - gTop.x(),
                                         globalLogical.y() - gTop.y()),
                   m_dragMime, actions, proposed, modifiers);
    jzApp->sendEvent(target, &ev);
    const bool accepted = ev.isAccepted();

    dragResetTarget(false);         // drop 之后没有"拖离"一说, 直接收尾
    m_dragAction = accepted ? jzResolvedDropAction(&ev) : JZDropActionNone;
    return accepted;
}

// 前置并激活: 未显示先 show (原生窗口惰性创建), 最小化则还原, 再交给系统做前台激活。
void JZWindow::activateWindow()
{
    if (!isVisible())
        show();            // 未创建的窗口不可能成为前台窗口
    if (!m_hwnd)
        return;

    HWND hwnd = (HWND)m_hwnd;
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);

    // 常规四连: Z 序提到最前(同线程) -> 争取前台 -> 同线程内强制激活(会发 WM_ACTIVATE)
    // -> 设键盘焦点(发 WM_SETFOCUS, 之后 isActiveWindow() 为 true)。多数情况到这里就够。
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);

    // 兜底: 系统"前台锁定"规则下(例如前台窗口刚被别的窗口抢走/焦点切换正在排队),
    // 上面的调用会被静默忽略。把自己的线程输入队列临时附到当前前台线程, 借它的
    // 前台权限完成激活, 随后立刻解开 —— Qt/Chromium 的同款做法(成对调用, 中途不返回)。
    if (GetForegroundWindow() != hwnd)
    {
        HWND fg = GetForegroundWindow();
        DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        DWORD myThread = GetCurrentThreadId();
        if (fgThread != 0 && fgThread != myThread && AttachThreadInput(fgThread, myThread, TRUE))
        {
            SetForegroundWindow(hwnd);
            SetActiveWindow(hwnd);
            SetFocus(hwnd);
            AttachThreadInput(fgThread, myThread, FALSE);
        }
    }

    // 立刻刷新应用级"最近激活窗口"(也是 activeWindow() 在整体失焦时的回退值);
    // 真正的活跃状态仍由 WM_SETFOCUS 回流后写入 m_hasWindowFocus。
    if (jzApp)
        jzApp->setActiveWindow(this);
}

// WM_SETFOCUS / WM_KILLFOCUS: 派发焦点进/出事件, 并重算 IMC 关联。
// 同 HWND 内多控件共享一个 IMC, 关联/断开一律走 updateImeContext(),
// 控件自身不再各自操作 (否则一个控件断开会影响同窗口其它控件)。
void JZWindow::onWindowFocus(bool got)
{
    m_hasWindowFocus = got;
    if (got && jzApp)
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

// IMC 关联: 窗口创建后**始终保持关联**, 只按焦点是否接受输入法来开关 IME 的
// UI 与组字 (m_imeActive)。
//
// 关键: 不能按焦点频繁 ImmAssociateContext(NULL) 断开。IMC 断开后 ImmGetContext
// 返回 NULL, 再次关联时输入法会把该窗口的输入状态重新初始化成默认值 —— 中文
// (native) 模式, 于是"一聚焦输入框输入法就自动从英文变中文"。断开期间用户在
// 别处切的中/英状态也同步不回来。
void JZWindow::updateImeContext()
{
    if (!m_hwnd)
        return;

    // 确保已缓存默认 IMC (正常情况 createWindow 时已缓存)
    if (!m_imeContext) {
        HIMC hImc = ImmGetContext((HWND)m_hwnd);
        if (!hImc) {
            // 已断开或从未关联: 先恢复默认关联再取
            ImmAssociateContextEx((HWND)m_hwnd, nullptr, IACE_DEFAULT);
            hImc = ImmGetContext((HWND)m_hwnd);
        }
        if (hImc) {
            m_imeContext = hImc;
            ImmReleaseContext((HWND)m_hwnd, hImc);
        }
    }

    // 只在"尚未关联 / 关联的 IMC 不对"时才真正关联一次: 重复关联同一个 IMC
    // 同样会触发 WM_IME_SETCONTEXT, 让输入法有机会重置状态。
    if (m_imeContext) {
        HIMC cur = ImmGetContext((HWND)m_hwnd);
        const bool attached = (cur != nullptr && cur == reinterpret_cast<HIMC>(m_imeContext));
        if (cur)
            ImmReleaseContext((HWND)m_hwnd, cur);
        if (!attached)
            ImmAssociateContext((HWND)m_hwnd, reinterpret_cast<HIMC>(m_imeContext));
    }

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

// 主动重发 WM_IME_SETCONTEXT。组字窗由控件自绘, 故永远不带 ISC_SHOWUICOMPOSITIONWINDOW。
void JZWindow::updateImeUi(bool show)
{
    if (!m_hwnd)
        return;

    const LPARAM lp = ISC_SHOWUIALL & ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW);
    SendMessageW((HWND)m_hwnd, WM_IME_SETCONTEXT,
                 show ? TRUE : FALSE, show ? lp : static_cast<LPARAM>(0));
}

// 放弃本次组字 (不上屏)
void JZWindow::cancelImeComposition()
{
    if (!m_hwnd)
        return;

    HIMC hImc = ImmGetContext((HWND)m_hwnd);
    if (hImc) {
        ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmReleaseContext((HWND)m_hwnd, hImc);
    }
}

// 让 IME 的组字窗/候选窗字体与本窗口一致。
// 字体族与 GDI+ 绘制侧保持一致 (JZGdiBackend::jzFontFamily)。
//
// 重要: 默认**完全不碰** IME 的 open/conversion 状态 (中英模式), 只设组字字体。
// 对微软拼音这类输入法, 英文模式在 IMM 层就表现为 open=false; 若这里无条件
// ImmSetOpenStatus(TRUE), 用户一切到英文、再聚焦任意输入框就会被强制拧回中文
// —— 表现正是"聚焦 lineEdit 输入法自动变中文"。IMC 已在窗口级全程保持关联,
// 语言栏/转换模式由系统与用户维护, 聚焦无需也不应改动。
//
// 仅当 m_imeForceNative (setImeForceNativeMode(true), "纯中文录入"场景) 才强制
// 打开并切到中文(native), 等价于参考实现 test_ime 的 kForceChineseOnFocus。
void JZWindow::prepareIme(int fontPixelSize)
{
    if (!m_hwnd)
        return;

    HIMC hImc = ImmGetContext((HWND)m_hwnd);
    if (!hImc)
        return;

    const int dpi = (int)jzQueryDpi((HWND)m_hwnd);

    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -MulDiv(fontPixelSize, dpi, 96);
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcscpy_s(lf.lfFaceName, L"Microsoft YaHei");
    ImmSetCompositionFontW(hImc, &lf);

    if (m_imeForceNative) {
        if (!ImmGetOpenStatus(hImc))
            ImmSetOpenStatus(hImc, TRUE);
        DWORD conv = 0, sent = 0;
        if (ImmGetConversionStatus(hImc, &conv, &sent)) {
            if ((conv & IME_CMODE_NATIVE) == 0) {
                conv |= IME_CMODE_NATIVE;
                ImmSetConversionStatus(hImc, conv, sent);
            }
        }
    }

    ImmReleaseContext((HWND)m_hwnd, hImc);
}

// ==================== 关闭流程 ====================
// Qt 语义的关闭: close() 同步派发 closeEvent (QApplication::sendEvent 习惯),
// 重写方以 accept/ignore 表态; 接受才真正关闭 (隐藏 + 销毁原生窗口)。
// WM_CLOSE -> close(), 业务代码也直接调 close() —— 没有单独的 requestClose。
// (JZDialog::closeEvent 会 ignore() 并转为 reject: 只隐藏、不销毁, 可复用。)
bool JZWindow::close()
{
    JZCloseEvent ev;
    jzApp->sendEvent(this, &ev);   // -> event(JZEvent_close) -> closeEvent(&ev)
    if (!ev.isAccepted())
        return false;                  // 重写方 ignore(): 拒绝关闭 (如对话框转 reject)

    // 接受关闭: 先 hide() 同步可见性, 再销毁原生窗口。两条路径都会走到
    // jzApp->notifyWindowClosed, 由它判定"最后一个可见顶层窗口已关"并退出
    // 事件循环。(只靠 WM_DESTROY 通知一次会漏: 主窗口在菜单弹层还开着时被
    // 关闭 —— 如文件→退出 —— 弹层随后才 hide, 便再没人通知, 进程残留。)
    hide();
    if (m_hwnd)
        DestroyWindow((HWND)m_hwnd);   // -> WM_DESTROY -> nativeDestroyed -> 再通知一次
    return true;
}

// 默认实现为空: JZCloseEvent 构造即 accepted, close() 据此执行关闭。
// 需要拦截关闭的窗口 (如 JZDialog) 重写后 ignore() 并自行处理。
void JZWindow::closeEvent(JZCloseEvent*)
{
}

// 原生窗口已销毁 (WM_DESTROY): 清宿主状态避免句柄悬空;
// 之后 show() 经 createWindow 会重新创建。
void JZWindow::nativeDestroyed()
{
    revokeDropTarget();     // 拖拽过程中窗口被销毁的路径: 先把 drop target 注销掉
    m_hwnd = nullptr;
    m_windowCreated = false;
    m_focusWidget = nullptr;
    m_hasWindowFocus = false;
    m_imeContext = nullptr;
    m_imeGotResult = false;
    m_imeActive = false;

    // 最后一个可见顶层窗口关闭 -> 退出事件循环 (jzApp->quit())。
    // 可用 jzApp->setQuitOnLastWindowClosed(false) 关掉这个行为。
    if (jzApp)
        jzApp->notifyWindowClosed(this);
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

// 屏幕逻辑坐标定位 (popup 等自定位窗口): 物理像素 = 逻辑 * scale。
// 惰性: 窗口未创建时只记录 (createWindow 时作为初始位置), 显式定位会关闭
// 显示自动居中 (等价 Qt 的 WA_Moved)。
void JZWindow::moveScreen(int x, int y)
{
    m_screenX = x;
    m_screenY = y;
    m_hasScreenPos = true;
    m_autoPosition = false;
    if (!m_hwnd || !m_device)
        return;
    float s = m_device->scale();
    SetWindowPos((HWND)m_hwnd, nullptr,
                 (int)std::lround(x * s), (int)std::lround(y * s),
                 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// ==================== 通用模态循环 ====================
// JZDialog::exec 与 popup 弹层共用。循环体位于 JZWin32Thread (与主循环共享
// processAndPaint 轮体: 泵 Win32 消息 + 框架事件 + 脏区绘制, owner 禁用/
// 恢复为 DialogBoxParam 语义), 此处仅转发, 控件层无需感知线程。
int JZWindow::execModal(bool disableOwner)
{
    auto* mt = static_cast<JZWin32Thread*>(jzApp->mainThread());
    return mt ? mt->execModal(this, disableOwner) : 0;
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

