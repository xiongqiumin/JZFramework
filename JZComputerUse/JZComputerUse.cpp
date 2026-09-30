#include "JZComputerUse.h"
#include <windows.h>
#include <map>
#include <vector>
#include <sstream>
#include <algorithm>
#include <shlobj.h>      // IShellDispatch4 / CLSID_Shell (显示桌面)
#include <objbase.h>     // CoInitializeEx / CoCreateInstance / IID_PPV_ARGS
#include <tlhelp32.h>    // 进程快照
#include <cstring>       // strstr
#include <cmath>         // lround (归一化坐标 -> 桌面像素)
// 框架侧(布局通道要用): 语义树 / 活动窗口 / 显示器几何 / 虚拟桌面后端
#include "JZApplication.h"
#include "JZLog.h"
#include "JZWidget/JZWidget.h"
#include "JZWidget/JZWindow.h"
#include "JZWidget/JZVirtualDevice.h"    // 虚拟桌面 (setVirtualDesktop)
#include "JZWidget/JZVirtualWindow.h"

// OpenCV 只在"像素族匹配"(matchImage/matchColor)里用，而像素族已降级为**兜底**
// (见 docs/compute-use-design.md §4.1) ⇒ 默认不编，避免为一个兜底能力背整条 OpenCV 依赖。
// 要启用: CMake 里加 `target_compile_definitions(<target> PRIVATE JZ_CU_OPENCV=1)` 并链 OpenCV。
#ifndef JZ_CU_OPENCV
#define JZ_CU_OPENCV 0
#endif
#if JZ_CU_OPENCV
#include <opencv2/opencv.hpp>
#endif

CuRect JZComputerUse::s_lastLogical;
JZVirtualDevice* JZComputerUse::s_virtualDesk = nullptr;

// —— 后端选择 ——
void JZComputerUse::setVirtualDesktop(JZVirtualDevice* desk) {
    s_virtualDesk = desk;
    // 归一化基准 = **整块桌面**: layoutTree 的归一化与 click 的反算用的是同一个
    // s_lastLogical, 这样"布局给的 rect"和"点击落点"必然同口径 (与真桌面路径同一条规矩)。
    if (desk)
        updateLogical(JZRect(0, 0, desk->desktopWidth(), desk->desktopHeight()));
}

JZVirtualDevice* JZComputerUse::virtualDesktop() { return s_virtualDesk; }
bool JZComputerUse::isVirtualDesktop() { return s_virtualDesk != nullptr; }

namespace {

// 虚拟桌面: 对外鼠标按钮 -> 框架鼠标事件按钮
JZMouseEvent::Button cuVirtualBtn(JZComputerUse::MouseBtn b) {
    if (b == JZComputerUse::MouseBtn::Right)  return JZMouseEvent::RightButton;
    if (b == JZComputerUse::MouseBtn::Middle) return JZMouseEvent::MiddleButton;
    return JZMouseEvent::LeftButton;
}

// 语义树的 boundsInScreen 对**虚拟窗口**而言是"窗口客户区坐标"(窗口不知道桌面偏移);
// 加上窗口的桌面坐标才是真正的"屏幕坐标" —— 与真实窗口 mapToGlobal 的语义对齐。
// (偏移由桌面提供: 坐标系归桌面所有, 窗口只知道自己那块客户区。)
void shiftBoundsInScreen(Json::Value& n, int dx, int dy) {
    Json::Value& b = n["boundsInScreen"];
    if (b.isArray() && b.size() >= 4) {
        b[0] = b[0].asInt() + dx;
        b[1] = b[1].asInt() + dy;
    }
    Json::Value& kids = n["children"];
    if (kids.isArray()) {
        for (auto& c : kids)
            shiftBoundsInScreen(c, dx, dy);
    }
}

#if JZ_CU_OPENCV
    std::string tempDir() {
        char buf[MAX_PATH];
        if (GetTempPathA(MAX_PATH, buf) > 0) return std::string(buf);
        return std::string(".\\");
    }
#endif

    void split(const std::string& s, char d, std::vector<std::string>& out) {
        std::string cur;
        for (char c : s) { if (c == d) { out.push_back(cur); cur.clear(); } else cur += c; }
        out.push_back(cur);
    }

    int vkFromName(const std::string& n) {
        static const std::map<std::string, int> m = {
            {"Return",VK_RETURN},{"Enter",VK_RETURN},{"Tab",VK_TAB},{"Escape",VK_ESCAPE},{"Esc",VK_ESCAPE},
            {"Backspace",VK_BACK},{"Delete",VK_DELETE},{"Space",VK_SPACE},{"Insert",VK_INSERT},
            {"Home",VK_HOME},{"End",VK_END},{"PageUp",VK_PRIOR},{"PageDown",VK_NEXT},
            {"Up",VK_UP},{"Down",VK_DOWN},{"Left",VK_LEFT},{"Right",VK_RIGHT},
            {"LWin",VK_LWIN},{"RWin",VK_RWIN},{"Ctrl",VK_CONTROL},{"Control",VK_CONTROL},
            {"Shift",VK_SHIFT},{"Alt",VK_MENU},{"Menu",VK_MENU},
            {"CapsLock",VK_CAPITAL},{"NumLock",VK_NUMLOCK},{"ScrollLock",VK_SCROLL},
            {"F1",VK_F1},{"F2",VK_F2},{"F3",VK_F3},{"F4",VK_F4},{"F5",VK_F5},{"F6",VK_F6},
            {"F7",VK_F7},{"F8",VK_F8},{"F9",VK_F9},{"F10",VK_F10},{"F11",VK_F11},{"F12",VK_F12},
        };
        auto it = m.find(n);
        if (it != m.end()) return it->second;
        if (n.size() == 1) {
            short v = VkKeyScanA((CHAR)n[0]);
            if (v != -1) return v & 0xFF;
        }
        return 0;
    }

#if JZ_CU_OPENCV
    // 解析 "#RRGGBB" / "RRGGBB" / "#RGB" 为 OpenCV BGR 标量
    cv::Scalar parseHex(const std::string& h) {
        std::string s = h;
        if (!s.empty() && s[0] == '#') s = s.substr(1);
        if (s.size() == 3) s = std::string(1,s[0])+s[0]+s[1]+s[1]+s[2]+s[2];
        unsigned int v = 0;
        std::stringstream ss; ss << std::hex << s; ss >> v;
        int r = (v>>16)&0xFF, g = (v>>8)&0xFF, b = v&0xFF;
        return cv::Scalar(b, g, r);
    }
#endif   // JZ_CU_OPENCV

    // —— 进程 / 窗口查找与置前（供 JZComputerUse 的进程窗口管理使用）——
    struct FindCtx { const std::string* title = nullptr; DWORD pid = 0; HWND found = nullptr; };

    BOOL CALLBACK EnumFindTitle(HWND w, LPARAM l) {
        auto* c = (FindCtx*)l;
        if (!IsWindowVisible(w)) return TRUE;
        char buf[512] = {};
        GetWindowTextA(w, buf, (int)sizeof(buf));
        if (strstr(buf, c->title->c_str())) { c->found = w; return FALSE; }
        return TRUE;
    }
    BOOL CALLBACK EnumFindPid(HWND w, LPARAM l) {
        auto* c = (FindCtx*)l;
        DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
        if (pid == c->pid && IsWindowVisible(w)) { c->found = w; return FALSE; }
        return TRUE;
    }
    // SetForegroundWindow 受限时, 借前台线程的输入队列提升成功率
    void bringToFront(HWND w) {
        if (!w) return;
        HWND fg = GetForegroundWindow();
        DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        DWORD myTid = GetCurrentThreadId();
        bool attached = fgTid && fgTid != myTid && AttachThreadInput(myTid, fgTid, TRUE) != 0;
        ShowWindow(w, SW_RESTORE);
        SetForegroundWindow(w);
        if (attached) AttachThreadInput(myTid, fgTid, FALSE);
    }
    // 宽转窄(UTF-8)：进程快照给的是 wchar_t，本类对外一律用 std::string(UTF-8)
    std::string narrow(const wchar_t* w) {
        if (!w || !*w) return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) return std::string();
        std::string s(n - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
        return s;
    }

    DWORD pidByName(const std::string& name) {
        DWORD pid = 0;
        if (name.empty()) return 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        // ⚠ 用 PROCESSENTRY32W/Process32FirstW（显式宽版）：`PROCESSENTRY32A` 在当前 SDK 里
        //   并没有 typedef（它只是 tag 名），而这个文件以前没进过构建、一直没暴露。
        PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
        for (BOOL b = Process32FirstW(snap, &pe); b; b = Process32NextW(snap, &pe)) {
            std::string p = narrow(pe.szExeFile);
            if (p == name || (name.back() != '.' && p == name + ".exe")) { pid = pe.th32ProcessID; break; }
        }
        CloseHandle(snap);
        return pid;
    }

    // ———— 布局通道的工具 ————

    // 虚拟桌面**逻辑**矩形 = 各显示器 geometry 的并集。
    // 与 captureVirtualDesktop(&rc, scale) 回填的 rc 同口径（那里面也是各屏 geometry 的并集，
    // 缩放不进 rc）⇒ 不抓图就能拿到，且与 click()/screenshot() 的归一化基准一致。
    JZRect virtualLogicalRect() {
        const int n = JZScreen::count();
        int l = 0, t = 0, r = 0, b = 0; bool first = true;
        for (int i = 0; i < n; i++) {
            JZScreen s(i);
            if (!s.isValid()) continue;
            JZRect g = s.geometry();
            if (g.width() <= 0 || g.height() <= 0) continue;
            if (first) {
                l = g.left(); t = g.top(); r = g.left() + g.width(); b = g.top() + g.height();
                first = false;
            } else {
                l = std::min(l, g.left()); t = std::min(t, g.top());
                r = std::max(r, g.left() + g.width()); b = std::max(b, g.top() + g.height());
            }
        }
        if (first)   // 拿不到显示器信息时退回 Win32 度量（物理像素，至少别给 0）
            return JZRect(0, 0, GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
        return JZRect(l, t, r - l, b - t);
    }

    // 屏幕逻辑矩形 → 归一化 [0,1]（相对虚拟桌面逻辑矩形；与 click() 同口径）
    Json::Value normRectJson(int x, int y, int w, int h, const JZRect& base) {
        const double bw = base.width() > 0 ? (double)base.width() : 1.0;
        const double bh = base.height() > 0 ? (double)base.height() : 1.0;
        double nx = (x - base.left()) / bw, ny = (y - base.top()) / bh;
        double nw = w / bw, nh = h / bh;
        if (nx < 0) { nw += nx; nx = 0; }
        if (ny < 0) { nh += ny; ny = 0; }
        if (nx + nw > 1) nw = 1 - nx;
        if (ny + nh > 1) nh = 1 - ny;
        if (nw < 0) nw = 0; if (nh < 0) nh = 0;
        Json::Value r(Json::objectValue);
        r["x"] = nx; r["y"] = ny; r["w"] = nw; r["h"] = nh;
        return r;
    }

    // JZWidget 语义树(dumpSemanticTree 的无障碍树) → 本包 LayoutNode 形态。
    // 字段一一对应: className→role / viewId→id / text→text / contentDesc→name /
    //              boundsInScreen→rect(归一化) / visible·enabled·clickable·checked·range·items·actions 直传。
    Json::Value widgetTreeToLayout(const Json::Value& n, const JZRect& base) {
        Json::Value o(Json::objectValue);
        o["role"] = n.isMember("className") ? n["className"].asString() : std::string("unknown");
        if (n.isMember("viewId")) o["id"] = n["viewId"];            // = objectName()（路径的稳定片段）
        if (n.isMember("text")) o["text"] = n["text"];
        if (n.isMember("contentDesc")) o["name"] = n["contentDesc"];
        const Json::Value& b = n["boundsInScreen"];
        if (b.isArray() && b.size() >= 4)
            o["rect"] = normRectJson(b[0].asInt(), b[1].asInt(), b[2].asInt(), b[3].asInt(), base);
        if (n.isMember("visible"))   o["visible"]   = n["visible"].asBool();
        if (n.isMember("enabled"))   o["enabled"]   = n["enabled"].asBool();
        if (n.isMember("clickable")) o["clickable"] = n["clickable"].asBool();
        if (n.isMember("checked"))   o["checked"]   = n["checked"].asBool();
        if (n.isMember("range"))     o["range"]     = n["range"];
        if (n.isMember("items"))     o["items"]     = n["items"];
        if (n.isMember("actions"))   o["actions"]   = n["actions"];
        const Json::Value& kids = n["children"];
        if (kids.isArray() && !kids.empty()) {
            Json::Value arr(Json::arrayValue);
            for (const auto& c : kids) arr.append(widgetTreeToLayout(c, base));
            o["children"] = arr;
        }
        return o;
    }

    // Win32 类名 → 本包词汇(粗映射, 只为人读得懂；路径仍以类名/id 为准)
    std::string roleFromClass(const std::string& cls) {
        std::string c = cls;
        for (auto& ch : c) ch = (char)tolower((unsigned char)ch);
        if (c == "button")     return "button";
        if (c == "edit")       return "edit";
        if (c == "static")     return "label";
        if (c == "combobox")   return "combobox";
        if (c == "syslistview32") return "list";
        if (c == "systreeview32") return "tree";
        if (c == "systabcontrol32") return "tab";
        if (c == "msctls_trackbar32") return "slider";
        if (c == "msctls_updown32")   return "spinbox";
        if (c == "msctls_progress32") return "progressbar";
        if (c == "systoolbar32") return "toolbar";
        if (c == "sstatusbar" || c == "msctls_statusbar32") return "statusbar";
        if (c == "scrollbar")  return "scrollbar";
        return c.empty() ? std::string("unknown") : c;
    }

    // 外部窗口: 递归枚举 Win32 子窗口 → LayoutNode。
    // ⚠ 只对"标准控件"有效(按钮/编辑框/静态文本/列表…); 自绘界面拿不到内部结构 ⇒
    //   那些节点的细节只能靠像素族兜底(或后续接 UIA)。
    Json::Value hwndTreeToLayout(HWND w, const JZRect& base) {
        Json::Value o(Json::objectValue);
        char cls[256] = {};
        GetClassNameA(w, cls, (int)sizeof(cls));
        char txt[512] = {};
        GetWindowTextA(w, txt, (int)sizeof(txt));
        // 顶层窗口没有 GetDlgCtrlID 语义 ⇒ 用类名当 role, 文本当 name(路径片段会退成 role)
        const bool top = (GetParent(w) == nullptr);
        o["role"] = top ? std::string("window") : roleFromClass(cls);
        if (!top) {
            int id = GetDlgCtrlID(w);
            if (id > 0) o["id"] = std::string("id") + std::to_string(id);   // 标准控件的稳定标识
        } else {
            o["id"] = roleFromClass(cls);        // 顶层窗口用类名当 id ⇒ 路径形如 /notepad
        }
        if (txt[0]) o["text"] = std::string(txt);
        RECT r = {};
        if (GetWindowRect(w, &r))
            o["rect"] = normRectJson(r.left, r.top, r.right - r.left, r.bottom - r.top, base);
        o["visible"]   = IsWindowVisible(w) != 0;
        o["enabled"]   = IsWindowEnabled(w) != 0;
        o["clickable"] = true;

        Json::Value arr(Json::arrayValue);
        for (HWND c = GetWindow(w, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            if (!IsWindowVisible(c)) continue;                    // 不可见的子窗口不进树(与自家语义树一致)
            arr.append(hwndTreeToLayout(c, base));
        }
        if (!arr.empty()) o["children"] = arr;
        return o;
    }
}

void JZComputerUse::updateLogical(const JZRect& r) {
    s_lastLogical = { (double)r.x(), (double)r.y(), (double)r.width(), (double)r.height() };
}

// 归一化(相对虚拟桌面逻辑矩形) -> 虚拟屏幕绝对坐标(0..65535, SendInput 用)
//
// ⚠ 坐标系必须统一：`s_lastLogical` 是**逻辑**虚拟桌面（JZScreen::geometry()），
//   而 DPI-aware 进程里 GetSystemMetrics(SM_*) 给的是**物理**像素 ⇒ 中间得乘 deviceScale。
//   不乘的后果（实机自检踩到）：200% 缩放下 rect 完全正确，但点击只落到目标的一半位置，
//   表现成"能定位、点不中" —— 这类 bug 只有真点一次才看得出来。
//   `MOUSEEVENTF_VIRTUALDESK` 让 0..65535 覆盖**整个虚拟桌面**（与 vsX/vsW 同口径），
//   多显示器下不设它会按主屏算，副屏上的坐标就偏了。
void JZComputerUse::toAbsolute(double nx, double ny, DWORD& ax, DWORD& ay) {
    int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsW = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    int vsH = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));

    // 归一化 → **逻辑**屏幕坐标（与 layoutTree 归一化用同一个 s_lastLogical ⇒ 能原路返回）
    double lx = s_lastLogical.x + nx * s_lastLogical.w;
    double ly = s_lastLogical.y + ny * s_lastLogical.h;

    // 逻辑 → 物理：按**该点所在显示器**的缩放。
    // ⚠ 别用 "物理虚拟屏 / 逻辑虚拟桌面" 当比例：`JZScreen::geometry()` 在多屏下会把副屏
    //   原点按物理值报(实测: 主屏 1440x900@2x + 副屏原点 2880 ⇒ 并集 4800x1080 是脏的)，
    //   拿它算出来的比例会是 1，于是 200% 屏上的点击只走一半 ⇒ "能定位、点不中"。
    //   改用该点所在屏的 devicePixelRatio；并用 "SM_CXSCREEN 是否大于主屏逻辑宽" 自校准
    //   进程是否 DPI-aware（非 DPI-aware 时系统已把坐标虚拟化，再乘就重复了）。
    double k = 1.0;
    JZScreen pri = JZScreen::primaryScreen();
    if (pri.isValid()) {
        JZRect pg = pri.geometry();
        const bool dpiAware = (GetSystemMetrics(SM_CXSCREEN) > pg.width() + 1);
        if (dpiAware) {
            JZScreen sc = JZScreen::screenAt(JZPoint((int)lx, (int)ly));
            if (sc.isValid() && sc.devicePixelRatio() > 0)
                k = sc.devicePixelRatio();
        }
    }

    double px = lx * k, py = ly * k;
    ax = (DWORD)(((px - vsX) / (double)vsW) * 65535.0);
    ay = (DWORD)(((py - vsY) / (double)vsH) * 65535.0);
}

DWORD JZComputerUse::btnDown(MouseBtn b) {
    if (b == MouseBtn::Right)  return MOUSEEVENTF_RIGHTDOWN;
    if (b == MouseBtn::Middle) return MOUSEEVENTF_MIDDLEDOWN;
    return MOUSEEVENTF_LEFTDOWN;
}
DWORD JZComputerUse::btnUp(MouseBtn b) {
    if (b == MouseBtn::Right)  return MOUSEEVENTF_RIGHTUP;
    if (b == MouseBtn::Middle) return MOUSEEVENTF_MIDDLEUP;
    return MOUSEEVENTF_LEFTUP;
}

void JZComputerUse::switchToDesktop() {
    // 虚拟桌面下没有"显示桌面"这回事 (桌面永远是它自己那块) —— 明确 no-op, 不碰真实系统
    if (s_virtualDesk)
        return;
    // 不用 Win+D 按键模拟(它是 toggle, 状态不可控); 改调 Explorer 真正的"显示桌面" COM 语义。
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellDispatch4* pShell = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_Shell, nullptr, CLSCTX_LOCAL_SERVER,
                                   IID_PPV_ARGS(&pShell)))) {
        pShell->MinimizeAll();   // 露出桌面(显示桌面)
        pShell->Release();
    }
    if (SUCCEEDED(hr)) CoUninitialize();
}

bool JZComputerUse::click(double nx, double ny, MouseBtn btn) {
    // 虚拟桌面: 归一化 -> 桌面像素 -> 命中窗口(并激活它) -> 换算客户区 -> 投递控件
    if (s_virtualDesk) {
        const int x = (int)std::lround(nx * s_virtualDesk->desktopWidth());
        const int y = (int)std::lround(ny * s_virtualDesk->desktopHeight());
        return s_virtualDesk->injectClickAt(x, y, cuVirtualBtn(btn));
    }
    DWORD ax, ay; toAbsolute(nx, ny, ax, ay);
    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE; in[0].mi.dx = ax; in[0].mi.dy = ay; in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in[1].type = INPUT_MOUSE; in[1].mi.dx = ax; in[1].mi.dy = ay; in[1].mi.dwFlags = btnDown(btn) | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    UINT n1 = SendInput(2, in, sizeof(INPUT));
    Sleep(20);
    in[1].mi.dwFlags = btnUp(btn) | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    UINT n2 = SendInput(2, in, sizeof(INPUT));
    // 注入被系统挡掉时(SendInput 返 <2, 常见于前台锁定/UIPI)要能看出来，别静默成功
    if (n1 < 2 || n2 < 2) {
        LOG_FMT_E("[CU] SendInput 只注入了 %u/%u 个事件（前台上锁 / UIPI？）", n1, n2);
        return false;
    }
    return true;
}

bool JZComputerUse::doubleClick(double nx, double ny, MouseBtn btn) {
    click(nx, ny, btn); Sleep(20); click(nx, ny, btn); return true;
}

bool JZComputerUse::drag(double x1, double y1, double x2, double y2) {
    // 虚拟桌面: 按下 -> 中间位移若干步 (控件靠 move 判拖拽) -> 抬起
    if (s_virtualDesk) {
        JZVirtualDevice* d = s_virtualDesk;
        const int W = d->desktopWidth(), H = d->desktopHeight();
        const int ax = (int)std::lround(x1 * W), ay = (int)std::lround(y1 * H);
        const int bx = (int)std::lround(x2 * W), by = (int)std::lround(y2 * H);
        d->injectMouseAtScreen(JZMouseEvent::Press, JZMouseEvent::LeftButton, ax, ay);
        // 抓取由控件接管 (grabMouse/m_mouseGrabber), 后续 move 会直发它 —— 与真实拖拽同一条路
        for (int i = 1; i <= 5; ++i) {
            const int xi = ax + (bx - ax) * i / 5;
            const int yi = ay + (by - ay) * i / 5;
            d->injectMouseAtScreen(JZMouseEvent::Move, JZMouseEvent::LeftButton, xi, yi);
        }
        d->injectMouseAtScreen(JZMouseEvent::Release, JZMouseEvent::LeftButton, bx, by);
        return true;
    }
    DWORD ax1, ay1, ax2, ay2; toAbsolute(x1, y1, ax1, ay1); toAbsolute(x2, y2, ax2, ay2);
    INPUT in[4] = {};
    in[0].type = INPUT_MOUSE; in[0].mi.dx = ax1; in[0].mi.dy = ay1; in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in[1].type = INPUT_MOUSE; in[1].mi.dx = ax1; in[1].mi.dy = ay1; in[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in[2].type = INPUT_MOUSE; in[2].mi.dx = ax2; in[2].mi.dy = ay2; in[2].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in[3].type = INPUT_MOUSE; in[3].mi.dx = ax2; in[3].mi.dy = ay2; in[3].mi.dwFlags = MOUSEEVENTF_LEFTUP | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(4, in, sizeof(INPUT));
    return true;
}

void JZComputerUse::key(const std::string& name) {
    // 虚拟桌面: "Ctrl+C" 拆成修饰键 + 主键 -> 注入活动窗口 (不走 SendInput)
    if (s_virtualDesk) {
        std::vector<std::string> parts; split(name, '+', parts);
        uint32_t mods = JZKeyEvent::NoModifier;
        int vk = 0;
        for (size_t i = 0; i < parts.size(); ++i) {
            std::string low = parts[i];
            for (auto& c : low) c = (char)tolower((unsigned char)c);
            if (i + 1 < parts.size()) {                 // 前缀都是修饰键
                if (low == "ctrl" || low == "control") mods |= JZKeyEvent::CtrlModifier;
                else if (low == "shift")               mods |= JZKeyEvent::ShiftModifier;
                else if (low == "alt")                 mods |= JZKeyEvent::AltModifier;
            } else {
                vk = vkFromName(parts[i]);              // 最后一个是主键
            }
        }
        if (vk == 0)
            return;
        s_virtualDesk->injectKey((uint32_t)vk, std::string(), mods, true);
        s_virtualDesk->injectKey((uint32_t)vk, std::string(), mods, false);
        return;
    }
    std::vector<std::string> parts; split(name, '+', parts);
    std::vector<int> vks; vks.reserve(parts.size());
    for (auto& p : parts) vks.push_back(vkFromName(p));
    INPUT in[16] = {}; int k = 0;
    for (int v : vks) { in[k].type = INPUT_KEYBOARD; in[k].ki.wVk = (WORD)v; k++; }
    if (k) SendInput(k, in, sizeof(INPUT));
    Sleep(20);
    for (int i = (int)vks.size() - 1; i >= 0; i--) {
        in[i].type = INPUT_KEYBOARD; in[i].ki.wVk = (WORD)vks[i]; in[i].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    if (k) SendInput(k, in, sizeof(INPUT));
}

void JZComputerUse::type(const std::string& text) {
    // 虚拟桌面: 逐码点注入活动窗口 (UTF-8 原样, 不经 SendInput 的宽字符转换)
    if (s_virtualDesk) {
        s_virtualDesk->injectText(text);
        return;
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (n <= 1) return;
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &w[0], n);
    for (wchar_t c : w) {
        INPUT in[2] = {};
        in[0].type = INPUT_KEYBOARD; in[0].ki.wScan = (WORD)c; in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1].type = INPUT_KEYBOARD; in[1].ki.wScan = (WORD)c; in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
    }
}

void JZComputerUse::wait(int ms) { if (ms > 0) Sleep(ms); }

std::string JZComputerUse::screenshot() {
    // 虚拟桌面: 直接拿桌面合成帧 (不抓屏) —— 不受遮挡/z 序/前台/多屏影响, 也不用拷屏
    if (s_virtualDesk) {
        JZImage img = s_virtualDesk->screenshot();
        updateLogical(JZRect(0, 0, s_virtualDesk->desktopWidth(), s_virtualDesk->desktopHeight()));
        return JZScreenShot::toDataUrl(img);
    }
    JZRect logical;
    JZImage img = JZScreenShot::captureVirtualDesktop(&logical);
    updateLogical(logical);
    return JZScreenShot::toDataUrl(img);
}

CuRect JZComputerUse::matchImage(const std::string& templatePath, const CuRect& region) {
#if !JZ_CU_OPENCV
    (void)templatePath; (void)region;
    return CuRect();        // OpenCV 未接入(像素族已降级为兜底, 见文件头) ⇒ 视为未找到
#else
    JZRect logical;
    JZImage img = JZScreenShot::captureVirtualDesktop(&logical);
    updateLogical(logical);
    std::string shotPath = JZScreenShot::save(img, tempDir(), "cu_shot.png");
    cv::Mat hay = cv::imread(shotPath, cv::IMREAD_COLOR);
    cv::Mat needle = cv::imread(templatePath, cv::IMREAD_COLOR);
    if (hay.empty() || needle.empty()) return CuRect();

    int hw = hay.cols, hh = hay.rows;
    cv::Rect roi(0, 0, hw, hh);
    if (region.w > 0 && region.h > 0) {
        int x0 = std::max(0, (int)(region.x * hw));
        int y0 = std::max(0, (int)(region.y * hh));
        int ww = std::min(hw - x0, (int)(region.w * hw));
        int hh2 = std::min(hh - y0, (int)(region.h * hh));
        if (ww <= 0 || hh2 <= 0) return CuRect();
        roi = cv::Rect(x0, y0, ww, hh2);
    }
    cv::Mat sub = hay(roi);
    if (sub.cols < needle.cols || sub.rows < needle.rows) return CuRect();

    cv::Mat result;
    cv::matchTemplate(sub, needle, result, cv::TM_CCOEFF_NORMED);
    double minv, maxv; cv::Point minLoc, maxLoc;
    cv::minMaxLoc(result, &minv, &maxv, &minLoc, &maxLoc);
    const double kThresh = 0.8;            // 单尺度, 阈值偏高以抑制误命
    if (maxv < kThresh) return CuRect();

    int cx = roi.x + maxLoc.x + needle.cols / 2;
    int cy = roi.y + maxLoc.y + needle.rows / 2;
    CuRect r;
    r.x = (double)cx / hw; r.y = (double)cy / hh;
    r.w = (double)needle.cols / hw; r.h = (double)needle.rows / hh;
    return r;
#endif
}

CuRect JZComputerUse::matchColor(const std::string& hexColor, const CuRect& region) {
#if !JZ_CU_OPENCV
    (void)hexColor; (void)region;
    return CuRect();        // 同上: 未接入 OpenCV ⇒ 视为未找到
#else
    JZRect logical;
    JZImage img = JZScreenShot::captureVirtualDesktop(&logical);
    updateLogical(logical);
    std::string shotPath = JZScreenShot::save(img, tempDir(), "cu_shot.png");
    cv::Mat hay = cv::imread(shotPath, cv::IMREAD_COLOR);
    if (hay.empty()) return CuRect();

    int hw = hay.cols, hh = hay.rows;
    cv::Rect roi(0, 0, hw, hh);
    if (region.w > 0 && region.h > 0) {
        int x0 = std::max(0, (int)(region.x * hw));
        int y0 = std::max(0, (int)(region.y * hh));
        int ww = std::min(hw - x0, (int)(region.w * hw));
        int hh2 = std::min(hh - y0, (int)(region.h * hh));
        if (ww <= 0 || hh2 <= 0) return CuRect();
        roi = cv::Rect(x0, y0, ww, hh2);
    }
    cv::Mat sub = hay(roi);
    cv::Scalar target = parseHex(hexColor);
    cv::Mat diff; cv::absdiff(sub, cv::Scalar(target[0], target[1], target[2]), diff);
    std::vector<cv::Mat> ch; cv::split(diff, ch);
    cv::Mat d0, d1, d2;
    ch[0].convertTo(d0, CV_32F); ch[1].convertTo(d1, CV_32F); ch[2].convertTo(d2, CV_32F);
    cv::Mat dist = d0 + d1 + d2;            // 三通道绝对差之和
    double tol = 60;                        // 容差(每通道平均 ~20)
    cv::Mat mask; cv::threshold(dist, mask, tol, 255, cv::THRESH_BINARY_INV);
    mask.convertTo(mask, CV_8U);
    std::vector<cv::Point> pts; cv::findNonZero(mask, pts);
    if (pts.empty()) return CuRect();
    int minx = INT_MAX, miny = INT_MAX, maxx = 0, maxy = 0;
    for (auto& p : pts) {
        if (p.x < minx) minx = p.x; if (p.y < miny) miny = p.y;
        if (p.x > maxx) maxx = p.x; if (p.y > maxy) maxy = p.y;
    }
    CuRect r;
    r.x = (double)(roi.x + minx) / hw; r.y = (double)(roi.y + miny) / hh;
    r.w = (double)(maxx - minx + 1) / hw; r.h = (double)(maxy - miny + 1) / hh;
    return r;
#endif
}

// ————————————————————————————————————————————————
// 布局通道（运行期寻址的**正路**）
// ————————————————————————————————————————————————
Json::Value JZComputerUse::layoutTree(const std::string& titleFilter) {
    // ---- 虚拟桌面: 目标 = 桌面上的窗口 (titleFilter 匹配不到就退回活动窗口) ----
    if (s_virtualDesk) {
        JZVirtualDevice* d = s_virtualDesk;
        JZVirtualWindow* w = titleFilter.empty() ? d->activeWindow() : d->byTitle(titleFilter);
        if (!w)
            return Json::Value();          // 没有目标窗口 => null (JS 侧据此标 degraded)

        const JZRect base(0, 0, d->desktopWidth(), d->desktopHeight());
        updateLogical(base);               // 归一化基准 = 整块桌面 (与 click 同口径)

        Json::Value tree = w->dumpSemanticTree(-1);
        // 窗口客户区坐标 -> 桌面屏幕坐标 (窗口自己不知道它在桌面上的位置)
        shiftBoundsInScreen(tree, w->screenX(), w->screenY());

        Json::Value out(Json::objectValue);
        out["source"] = "semantic";
        out["tree"] = widgetTreeToLayout(tree, base);
        return out;
    }

    // 目标窗口: 指定了标题就按标题找(自动化脚本多半在终端里跑, "前台窗口"常常是终端本身),
    // 否则退回前台窗口。
    HWND target = nullptr;
    if (!titleFilter.empty()) {
        FindCtx c; c.title = &titleFilter;
        EnumWindows(EnumFindTitle, (LPARAM)&c);
        target = c.found;
    }
    if (!target) target = GetForegroundWindow();
    HWND fg = target;
    if (!fg || !IsWindow(fg)) return Json::Value();      // 没有目标窗口 ⇒ null(JS 侧据此标 degraded)

    const JZRect base = virtualLogicalRect();
    updateLogical(base);        // ⚠ 顺手把归一化基准对齐到"当前"虚拟桌面 —— click() 用的就是它，
                                //   否则布局给的 rect 与点击落点会差一个桌面偏移(多屏时尤其明显)。

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) {
        // 自家界面: 走框架语义树(带 viewId ⇒ 布局路径稳定, 这才是"运行期应该走的那条路")
        JZApplication* app = JZApplication::instance();
        JZWindow* w = app ? app->activeWindow() : nullptr;
        if (w) {
            Json::Value out(Json::objectValue);
            out["source"] = "semantic";
            out["tree"] = widgetTreeToLayout(w->dumpSemanticTree(-1), base);
            return out;
        }
        // 前台是本进程的窗口但框架拿不到活动窗口(极少见) ⇒ 退化到 Win32 枚举, 不假装成功
    }

    // 外部软件: 递归枚举 Win32 子窗口(标准控件够用; 自绘控件只能靠像素兜底/UIA, 后续)
    Json::Value out(Json::objectValue);
    out["source"] = "win32";
    out["tree"] = hwndTreeToLayout(fg, base);
    return out;
}

bool JZComputerUse::waitStable(int settleMs, int timeoutMs) {
    // 虚拟桌面: 注入是**同步**的 (命令返回时那一帧已经渲染完), 不存在"界面还没渲染完" —— 恒稳。
    // 这正是确定性回放的关键: 真桌面要靠"帧间变化收敛"去猜, 这里不用猜。
    if (s_virtualDesk)
        return true;
    if (settleMs < 0) settleMs = 300;
    if (timeoutMs <= 0) timeoutMs = 10000;
    const int step = 100;

    // 像素在这里**只当信号**: 连续两帧"缩略图相同"就认为画面没在变。
    // 比对用缩略图的 PNG 字节(不引图像比较 API, 也不用 OpenCV)。
    auto fingerprint = [](std::string& out) -> bool {
        JZRect rc;
        JZImage img = JZScreenShot::captureVirtualDesktop(&rc);
        if (img.isNull()) return false;
        updateLogical(rc);
        JZByteArray png = JZScreenShot::toPng(JZScreenShot::fit(img, 96));   // 缩到 96px 长边再编码
        out.assign(png.data(), png.size());
        return true;
    };

    std::string prev, cur;
    if (!fingerprint(prev)) return false;
    int sameMs = 0, waited = 0;
    while (waited < timeoutMs) {
        wait(step); waited += step;
        if (!fingerprint(cur)) return false;
        if (cur == prev) { sameMs += step; } else { sameMs = 0; prev = cur; }
        if (sameMs >= settleMs) return true;
    }
    return false;              // 超时: 一直没稳定(动画/闪烁/高负载)
}

bool JZComputerUse::launch(const std::string& exe, const std::string& args,
                           const std::string& title, int timeoutMs) {
    // 虚拟桌面: 不启动真实进程 —— 桌面上的窗口由宿主创建 (切目标用 focusWindow)
    if (s_virtualDesk)
        return false;
    if (!exe.empty() && startProcess(exe, args) == 0)
        return false;

    if (title.empty()) {
        if (!exe.empty()) { wait(800); return true; }    // 只启动: 给它一点时间起窗口
        return false;                                    // 既没 exe 也没 title ⇒ 没目标可做
    }

    const int limit = timeoutMs > 0 ? timeoutMs : 8000;
    int waited = 0;
    while (waited <= limit) {
        if (focusWindow(title)) return true;             // 窗口出现并置前 = 目标就绪
        wait(200); waited += 200;
    }
    return false;
}


// ————————————————————————————————————————————————
// 进程与窗口管理（不使用按键模拟）
// ————————————————————————————————————————————————
DWORD JZComputerUse::startProcess(const std::string& path, const std::string& args) {
    // 虚拟桌面: 绝不启动真实进程
    if (s_virtualDesk)
        return 0;
    STARTUPINFOA si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::string cmd = path;
    if (!args.empty()) { cmd += " "; cmd += args; }
    std::vector<char> buf(cmd.c_str(), cmd.c_str() + cmd.size() + 1); // 含结尾 '\0'
    if (CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, nullptr, &si, &pi)) {
        DWORD pid = pi.dwProcessId;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return pid;
    }
    return 0;
}

bool JZComputerUse::terminateProcess(DWORD pid) {
    // 虚拟桌面: 绝不去杀真实进程 (要"关掉桌面上那个窗口"用 JZVirtualDevice::destroyWindow)
    if (s_virtualDesk)
        return false;
    HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return false;
    bool ok = TerminateProcess(h, 0) != 0;
    CloseHandle(h);
    return ok;
}

bool JZComputerUse::terminateProcess(const std::string& name) {
    DWORD pid = pidByName(name);
    if (!pid) return false;
    return terminateProcess(pid);
}

bool JZComputerUse::focusProcess(DWORD pid) {
    // 虚拟桌面: 没有"进程"这一层 —— 切目标请用 focusWindow
    if (s_virtualDesk)
        return false;
    FindCtx c; c.pid = pid;
    EnumWindows(EnumFindPid, (LPARAM)&c);
    if (!c.found) return false;
    bringToFront(c.found);
    return true;
}

bool JZComputerUse::focusProcess(const std::string& name) {
    DWORD pid = pidByName(name);
    if (!pid) return false;
    return focusProcess(pid);
}

bool JZComputerUse::focusWindow(const std::string& title) {
    // 虚拟桌面: 激活桌面上标题匹配的那个窗口 (置顶 + 键盘焦点)
    if (s_virtualDesk) {
        JZVirtualWindow* w = s_virtualDesk->byTitle(title);
        if (!w)
            return false;
        s_virtualDesk->activate(w);
        return true;
    }
    FindCtx c; c.title = &title;
    EnumWindows(EnumFindTitle, (LPARAM)&c);
    if (!c.found) return false;
    bringToFront(c.found);
    // ⚠ 返回值改成"**是否真的成了前台**"（原来"找到了就返回 true"）。
    //   Win32 的前台切换是有规则的（后台进程 SetForegroundWindow 可能只闪一下任务栏），
    //   而"窗口在前台"是**注入点击的前提** —— 不在前台时点击会落到上层窗口上，
    //   表现成"能定位、点不中且不报错"。调用方(脚本)可以据此重试。
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    return GetAncestor(fg, GA_ROOT) == GetAncestor(c.found, GA_ROOT);
}
