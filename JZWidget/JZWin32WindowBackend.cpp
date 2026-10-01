// ==================== Windows 真窗口后端 ====================
// 本文件是"JZWidget 的平台代码"在 Windows 上的**唯一边界**:
// 注册窗口类 / 创建窗口 / 窗口过程(WndProc) / 鼠标抓取 / 光标 / DPI / 输入法(IMM) /
// 消息循环(JZWin32Thread)。JZWindow.cpp 里已经没有一处 Win32 调用。
//
// 分工约定(与 JZVirtualWindowBackend 完全同构):
//   · 后端只做"平台动作"; **框架语义**(焦点控件/悬停/输入蒙版/命中测试/关闭事件)
//     一律留在 JZWindow —— 所以两个平台的行为不会漂移。
//   · 坐标口径: 客户区逻辑坐标 <-> 屏幕逻辑坐标, 物理像素换算(×scale)只在本文件里。
#include "JZWin32WindowBackend.h"
#include "JZWindow.h"
#include "JZDisplayDevice.h"
#include "JZApplication.h"
#include "JZWin32Thread.h"
#include "JZScreen.h"
#include "JZPainter.h"
#include "JZImeEvent.h"
#include "JZMouseEvent.h"
#include "JZKeyEvent.h"
#include "JZMimeData.h"
#include "JZDragEvent.h"

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
#include <objbase.h>      // CoTaskMemFree / ReleaseStgMedium
#include <oleidl.h>       // IDropTarget / IDataObject / RegisterDragDrop
#include <shellapi.h>     // DragQueryFileW (CF_HDROP)

// ==================== 平台辅助 ====================

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

// ==================== 窗口过程 ====================
// 平台翻译层: 输入类消息 (鼠标/键盘/字符/IME/窗口焦点) 翻译成 JZEvent 后, 统一交给
// JZWindow::onWindowProcess 分发到控件树; 窗口生命周期消息 (WM_SIZE / WM_DPICHANGED /
// WM_PAINT / WM_CLOSE) 不对应 JZEvent 类型, 直接处理。
//
// ⚠ 这里只调 JZWindow 的**公有**接口 —— 后端与框架的边界就在这层:
//   "翻译消息"是平台的事, "事件怎么走"是框架的事。
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

// ==================== JZWin32WindowBackend ====================

JZWin32WindowBackend::JZWin32WindowBackend()
{
}

JZWin32WindowBackend::~JZWin32WindowBackend()
{
    // 正常路径由 JZWindow::destroy() / ~JZWindow 调 destroy(); 这里只兜底清句柄
    if (m_hwnd) {
        DestroyWindow((HWND)m_hwnd);
        m_hwnd = nullptr;
    }
}

// 惰性创建: 原生窗口 + 渲染设备初始化 (首次 showEvent 触发, 幂等由 JZWindow 保证)。
// 尺寸/标题取当前值 (构造后可能被 resize/setTitle 调整过)。
bool JZWin32WindowBackend::create(JZWindow* w)
{
    if (!w || m_hwnd)
        return m_hwnd != nullptr;

    const int cw = w->width();
    const int ch = w->height();

    // 初次设定 DPI 缩放: 已显式设定 (popup 跟随 owner 显示器) 则保留;
    // 否则优先取 owner 所在显示器 DPI, 没有 owner 再退主屏 DPI (多屏/HiDPI 下
    // 弹层与 owner 才在同一坐标系)。
    if (!w->m_scaleExplicit && w->m_device) {
        HWND owner = nullptr;
        if (JZWidget* p = w->parentWidget()) {
            if (JZWindow* pw = p->window())
                owner = (HWND)pw->nativeHandle();
        }
        w->m_device->setScale(JZScreen::logicalDpiFor(owner) / 96.0f);
    }

    registerWindowClass();

    DWORD style = 0;
    DWORD exStyle = 0;
    switch (w->m_windowType) {
    case JZWindow::Modal:
        style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;   // 对话框框形 (无最小/最大化)
        break;
    case JZWindow::Popup:
        style = WS_POPUP;
        exStyle = WS_EX_TOOLWINDOW;   // 无边框浮层, 不进任务栏
        break;
    case JZWindow::Normal:
    default:
        style = WS_OVERLAPPEDWINDOW;
        break;
    }
    const float scale = w->m_device ? w->m_device->scale() : 1.0f;
    RECT rc = jzWindowRectForClient(nullptr, style, cw, ch, scale);

    // 顶层窗口的 hWndParent 即 owner (DialogBoxParam 模态 / Z 序 / 共命运的基础):
    // 取父链上所属窗口 (对话框以父级所在的窗口为 owner)。
    HWND owner = nullptr;
    if (JZWidget* p = w->parentWidget()) {
        if (JZWindow* pw = p->window())
            owner = (HWND)pw->nativeHandle();
    }

    // 初始位置 (等价 Qt: create_sys 用 crect 初始化, 出现即就位无跳变):
    //   1. 显式 moveScreen 记录的屏幕逻辑坐标 (WA_Moved 等价) —— 优先;
    //   2. Modal/Popup 自动定位: 相对 owner 窗口居中;
    //   3. 其余 (Normal 主窗口等) CW_USEDEFAULT 交给 Windows 级联。
    int posX = CW_USEDEFAULT, posY = CW_USEDEFAULT;
    if (w->m_hasScreenPos) {
        posX = (int)std::lround(w->m_screenX * scale);
        posY = (int)std::lround(w->m_screenY * scale);
    } else if (w->m_autoPosition && w->m_windowType != JZWindow::Normal && owner) {
        RECT orc;
        if (GetWindowRect(owner, &orc)) {
            int physW = rc.right - rc.left, physH = rc.bottom - rc.top;
            posX = orc.left + ((orc.right - orc.left) - physW) / 2;
            posY = orc.top + ((orc.bottom - orc.top) - physH) / 2;
        }
    }

    std::wstring wtitle = jzUtf8ToUtf16(w->m_title);
    m_hwnd = CreateWindowExW(exStyle, L"JZWindowWndClass", wtitle.c_str(), style,
                             posX, posY,
                             rc.right - rc.left, rc.bottom - rc.top,
                             owner, nullptr, GetModuleHandleW(nullptr), w);
    if (!m_hwnd)
        return false;

    unsigned physW = (unsigned)std::lround(cw * scale);
    unsigned physH = (unsigned)std::lround(ch * scale);
    if (w->m_device)
        w->m_device->init(m_hwnd, physW, physH);
    if (w->m_top)
        SetWindowPos((HWND)m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

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
    return true;
}

void JZWin32WindowBackend::destroy(JZWindow* w)
{
    revokeDragAndDrop(w);   // 句柄销毁前先注销 (WM_DESTROY 路径里也会再 revoke 一次, 幂等)
    if (m_hwnd) {
        DestroyWindow((HWND)m_hwnd);   // -> WM_DESTROY -> JZWindow::nativeDestroyed()
        m_hwnd = nullptr;
    }
    m_imeContext = nullptr;
}

void JZWin32WindowBackend::onPlatformDestroyed(JZWindow* w)
{
    (void)w;
    revokeDragAndDrop(w);
    m_hwnd = nullptr;
    m_imeContext = nullptr;
}

bool JZWin32WindowBackend::isCreated() const
{
    return m_hwnd != nullptr;
}

void* JZWin32WindowBackend::handle() const
{
    return m_hwnd;
}

void JZWin32WindowBackend::show(JZWindow* w)
{
    (void)w;
    if (!m_hwnd)
        return;
    ShowWindow((HWND)m_hwnd, SW_SHOW);
    // Popup 不再 SetCapture: 抓取是全局唯一的, 嵌套弹层(子菜单)会抢走抓取,
    // 父弹层从此收不到鼠标消息(悬停/选择失效)。"外部按下关闭"由消息泵在
    // 派发按下消息前统一判定, 见 JZWindow::filterPopupPress。
}

void JZWin32WindowBackend::hide(JZWindow* w)
{
    (void)w;
    if (!m_hwnd)
        return;
    // 只有抓取确实在本窗口手里时才释放, 避免误放别的窗口(或子弹层)的抓取
    if (GetCapture() == (HWND)m_hwnd)
        ReleaseCapture();
    ShowWindow((HWND)m_hwnd, SW_HIDE);
}

void JZWin32WindowBackend::setTitle(JZWindow* w, const std::string& title)
{
    (void)w;
    if (m_hwnd)
        SetWindowTextW((HWND)m_hwnd, jzUtf8ToUtf16(title).c_str());
}

void JZWin32WindowBackend::setClientSize(JZWindow* w, int cw, int ch)
{
    if (!m_hwnd || !w || !w->device())
        return;
    DWORD style = (DWORD)GetWindowLongPtrW((HWND)m_hwnd, GWL_STYLE);
    RECT rc = jzWindowRectForClient((HWND)m_hwnd, style, cw, ch, w->device()->scale());
    SetWindowPos((HWND)m_hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);   // WM_SIZE -> windowResized
    w->update();
}

void JZWin32WindowBackend::setScreenPos(JZWindow* w, int x, int y)
{
    if (!m_hwnd || !w || !w->device())
        return;
    const float s = w->device()->scale();
    SetWindowPos((HWND)m_hwnd, nullptr,
                 (int)std::lround(x * s), (int)std::lround(y * s),
                 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

JZPoint JZWin32WindowBackend::clientToScreen(JZWindow* w, const JZPoint& pt) const
{
    // 与 Qt 一致: 客户区逻辑坐标 -> 屏幕逻辑坐标。
    // ×scale -> ClientToScreen (物理像素) -> ÷scale。没有原生窗口时无从换算, 原样返回。
    if (!m_hwnd || !w || !w->device())
        return pt;
    const float s = w->device()->scale();
    POINT p = { (LONG)std::lround(pt.x() * s), (LONG)std::lround(pt.y() * s) };
    ClientToScreen((HWND)m_hwnd, &p);
    return JZPoint((int)std::lround(p.x / s), (int)std::lround(p.y / s));
}

JZPoint JZWin32WindowBackend::screenToClient(JZWindow* w, const JZPoint& pt) const
{
    if (!m_hwnd || !w || !w->device())
        return pt;
    const float s = w->device()->scale();
    POINT p = { (LONG)std::lround(pt.x() * s), (LONG)std::lround(pt.y() * s) };
    ScreenToClient((HWND)m_hwnd, &p);
    return JZPoint((int)std::lround(p.x / s), (int)std::lround(p.y / s));
}

// 本窗口所在显示器的缩放 (没建窗口时按主屏)
float JZWin32WindowBackend::deviceScale(JZWindow* w) const
{
    (void)w;
    return JZScreen::logicalDpiFor(m_hwnd) / 96.0f;
}

bool JZWin32WindowBackend::containsScreenPoint(JZWindow* w, int x, int y) const
{
    (void)w;
    if (!m_hwnd)
        return false;
    POINT pt = { (LONG)x, (LONG)y };
    RECT rc;
    return GetWindowRect((HWND)m_hwnd, &rc) && PtInRect(&rc, pt);
}

// 置顶: 已创建的原生窗口立即生效 (未创建时由 create() 套用 m_top)
void JZWin32WindowBackend::setTop(JZWindow* w, bool on)
{
    (void)w;
    if (!m_hwnd)
        return;
    SetWindowPos((HWND)m_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void JZWin32WindowBackend::showMinimized(JZWindow* w)
{
    (void)w;
    if (m_hwnd)
        ShowWindow((HWND)m_hwnd, SW_MINIMIZE);   // -> WM_SIZE (SIZE_MINIMIZED 被忽略)
}

void JZWin32WindowBackend::showMaximized(JZWindow* w)
{
    (void)w;
    if (m_hwnd)
        ShowWindow((HWND)m_hwnd, SW_MAXIMIZE);
}

void JZWin32WindowBackend::showNormal(JZWindow* w)
{
    (void)w;
    if (m_hwnd)
        ShowWindow((HWND)m_hwnd, SW_RESTORE);
}

// 前置并激活: 未显示先 show (原生窗口惰性创建), 最小化则还原, 再交给系统做前台激活。
void JZWin32WindowBackend::activate(JZWindow* w)
{
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
}

// 按当前光标位置 SetCursor: 取目标控件的生效光标 JZWidget::cursor()
// (自身 setCursor 或沿父链继承, 全无则箭头)。目标 = 拖拽抓取中的 grabber,
// 否则为光标下的最深控件 (命中为空以窗口自身兜底)。
// 被 WM_SETCURSOR (客户区内) 调用; setCursor / 鼠标派发后需要即时刷新时也会调。
void JZWin32WindowBackend::updateCursor(JZWindow* w)
{
    if (!m_hwnd || !w || !w->device())
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

    const float s = w->device()->scale();
    JZPoint g((int)std::lround(pt.x / s), (int)std::lround(pt.y / s));

    // 拖拽抓取中: 光标以 grabber 为准 (拖动过程不因光标移出控件命中带变回箭头)
    JZWidget* target = w->mouseGrabber() ? w->mouseGrabber() : w->hitTest(g);
    if (!target)
        target = w;

    // 全屏蒙版期间: 蒙版下方一律用窗口默认箭头, 不露出下层控件的光标
    // (如滑块的大小光标); 蒙版自身子控件 (取消按钮) 仍按其生效光标显示。
    if (w->inputGate() && w->inputGate()->isVisible() && target != w->inputGate()) {
        bool inGate = false;
        for (JZWidget* p = target; p; p = p->parentWidget()) {
            if (p == w->inputGate()) { inGate = true; break; }
        }
        if (!inGate)
            target = w;
    }

    SetCursor(jzCursorHandle(target->cursor().shape()));
}

// 鼠标抓取: on=true 抢系统抓取(含客户区外的 move/up 都路由到本窗口), false 时
// 只在抓取确实在本窗口手里才释放 (避免误放别的窗口/子弹层的抓取)。
void JZWin32WindowBackend::setCapture(JZWindow* w, bool on)
{
    (void)w;
    if (!m_hwnd)
        return;
    if (on)
        SetCapture((HWND)m_hwnd);
    else if (GetCapture() == (HWND)m_hwnd)
        ReleaseCapture();
}

// 脏区排进主线程的 paint task: 主线程的消息循环 (JZWin32Thread) 每轮统一画一次
// (与 WM_PAINT 同款)。真窗口走它; 虚拟桌面上"与宿主同线程"的窗口也走它
// (见 JZVirtualWindowBackend::onInvalidate —— 跨线程的窗口才发到那个进程去)。
// ⚠ 只有平台实现认识 JZWin32Thread: 后端接口(JZWindowBackend.h)和 JZWindow 都看不见平台类型。
void jzSchedulePaintOnMainThread(JZWindow* w, const JZRect& rc)
{
    auto* mt = static_cast<JZWin32Thread*>(jzApp->mainThread());
    mt->invalidate(w, rc);
}

int JZWin32WindowBackend::execModal(JZWindow* w, bool disableOwner)
{
    auto* mt = static_cast<JZWin32Thread*>(jzApp->mainThread());
    return mt ? mt->execModal(w, disableOwner) : 0;
}

// 客户区一帧 (逻辑尺寸): 抓屏后由 JZWidget::grab 按控件矩形裁剪
JZImage JZWin32WindowBackend::grabClient(JZWindow* w)
{
    (void)w;
    if (!m_hwnd)
        return JZImage();
    return JZScreen::grabWindow(m_hwnd);
}

// ==================== 输入法 (IMM) ====================

// IMC 关联: 窗口创建后**始终保持关联**, 只按焦点是否接受输入法来开关 IME 的
// UI 与组字 (m_imeActive 由 JZWindow 维护)。
//
// 关键: 不能按焦点频繁 ImmAssociateContext(NULL) 断开。IMC 断开后 ImmGetContext
// 返回 NULL, 再次关联时输入法会把该窗口的输入状态重新初始化成默认值 —— 中文
// (native) 模式, 于是"一聚焦输入框输入法就自动从英文变中文"。断开期间用户在
// 别处切的中/英状态也同步不回来。
void JZWin32WindowBackend::imeAssociate(JZWindow* w)
{
    (void)w;
    if (!m_hwnd)
        return;

    // 确保已缓存默认 IMC (正常情况 create 时已缓存)
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
        const bool attached = (cur != nullptr && cur == (HIMC)m_imeContext);
        if (cur)
            ImmReleaseContext((HWND)m_hwnd, cur);
        if (!attached)
            ImmAssociateContext((HWND)m_hwnd, (HIMC)m_imeContext);
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
void JZWin32WindowBackend::imePrepare(JZWindow* w, int fontPixelSize)
{
    if (!m_hwnd || !w)
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

    if (w->imeForceNativeMode()) {
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

// 放弃本次组字 (不上屏)
void JZWin32WindowBackend::imeCancel(JZWindow* w)
{
    (void)w;
    if (!m_hwnd)
        return;

    HIMC hImc = ImmGetContext((HWND)m_hwnd);
    if (hImc) {
        ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmReleaseContext((HWND)m_hwnd, hImc);
    }
}

// 主动重发 WM_IME_SETCONTEXT。组字窗由控件自绘, 故永远不带 ISC_SHOWUICOMPOSITIONWINDOW。
void JZWin32WindowBackend::imeUpdateUi(JZWindow* w, bool show)
{
    (void)w;
    if (!m_hwnd)
        return;

    const LPARAM lp = ISC_SHOWUIALL & ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW);
    SendMessageW((HWND)m_hwnd, WM_IME_SETCONTEXT,
                 show ? TRUE : FALSE, show ? lp : static_cast<LPARAM>(0));
}

// ==================== 拖放 (OLE IDropTarget) ====================
// 每个窗口把自己登记成一个 IDropTarget, 于是资源管理器/浏览器等外部数据源可以把
// 文件、图片、文本拖进来。OLE 的四个回调在这里翻译成 JZDrag*Event, 交给 JZWindow 的
// dragEnter/dragOver/dragDrop 按控件树分发 (命中 + 父链上抛, 同 Qt)。
//
// 选 IDropTarget 而不是 WM_DROPFILES: 后者只认文件列表, 浏览器直接拖图片
// (CF_DIB/CF_HBITMAP)、跨应用的自定义格式一律拿不到; RegisterDragDrop 才是 Qt 底层
// 走的那条路 (Qt 的 QWindowsDropTarget 同样如此)。

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

// 登记成系统拖放目标 (外部拖文件/图片进来)。句柄已创建时才调。
void JZWin32WindowBackend::registerDragAndDrop(JZWindow* w)
{
    if (!m_hwnd || m_dropTarget)
        return;

    // OLE 初始化一次即可 (进程级)。失败不影响窗口本身 —— 只是不能接收拖放。
    if (!jzEnsureOle())
        return;

    JZWindowDropTarget* dt = new JZWindowDropTarget(w);
    if (SUCCEEDED(RegisterDragDrop((HWND)m_hwnd, dt)))
        m_dropTarget = dt;
    else
        dt->Release();
}

// 注销 OLE drop target。必须在窗口句柄销毁**之前** (RevokeDragDrop 找不到已销毁的句柄)。
void JZWin32WindowBackend::revokeDragAndDrop(JZWindow* w)
{
    (void)w;
    if (m_dropTarget) {
        if (m_hwnd)
            RevokeDragDrop((HWND)m_hwnd);
        ((JZWindowDropTarget*)m_dropTarget)->Release();
        m_dropTarget = nullptr;
    }
}

// ==================== 默认后端工厂 ====================
JZWindowBackend* jzCreateDefaultWindowBackend()
{
    return new JZWin32WindowBackend();
}
