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

JZWindow::JZWindow(JZWidget* parent)
    : JZWidget(parent)
{
    // 默认尺寸
    m_device = new JZDisplayDevice();

    JZWidget::resize(800, 600);   // non-virtual: avoid ctor-time virtual dispatch

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
    // globalPos 换算为屏幕逻辑坐标 (Qt globalPos 语义), localPos 由 dispatchMouse 命中后回填
    JZMouseEvent ev(t, b, self->mapToGlobal(jzPhysicalToLogical(lParam, self->device()->scale())),
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

    // ---- popup 型窗口: 鼠标抓取期间, 客户区外的"按下" -> 关闭并吞掉 ----
    // (仅按下关闭, 吞掉该点击, 避免点 combo 自身出现"关闭又立刻重开"的 toggle;
    //  注意: 不能对 WM_MOUSEMOVE 关窗 —— 菜单弹出后光标从标题移到下拉项途中
    //  必然经过客户区外的间隙, 一旦按 move 关窗菜单立刻消失)
    if (self->windowType() == JZWindow::Popup &&
        (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN)) {
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
        // WM_MOUSEWHEEL 的 lParam 为屏幕物理像素坐标 -> 直接 /scale 即屏幕逻辑坐标
        // (Qt globalPos 语义); 不再 ScreenToClient。
        POINT pt = { (int16_t)LOWORD(lParam), (int16_t)HIWORD(lParam) };
        float s = self->device()->scale();
        JZMouseEvent ev(JZMouseEvent::Wheel, JZMouseEvent::NoButton,
                        JZPoint((int)std::lround(pt.x / s), (int)std::lround(pt.y / s)),
                        JZPoint(), GET_WHEEL_DELTA_WPARAM(wParam) / 120);
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
        self->requestClose();   // 走框架关闭流程 (closeEvent 可拦截, 不直接销毁)
        return 0;

    case WM_DESTROY:
        self->nativeDestroyed();
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rect;
        GetClientRect(hwnd, &rect);

        // 创建红色画刷并填充
        HBRUSH hBrush = CreateSolidBrush(RGB(255, 0, 0));
        FillRect(hdc, &rect, hBrush);
        DeleteObject(hBrush);  // 及时释放资源

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
        // 窗口隐藏: 任何进行中的鼠标抓取都失效 (控件可能已被移除/不可见)
        if (m_mouseGrabber)
            releaseMouse(nullptr);
        if (m_windowType == Popup)
            ReleaseCapture();
        ShowWindow((HWND)m_hwnd, SW_HIDE);
    }
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

