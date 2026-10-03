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
#include <cstdio>        // snprintf (窗口句柄字符串)
#include <cctype>        // isdigit (窗口标识解析)
#include <cmath>         // lround (归一化坐标 -> 桌面像素)
#include <cstdlib>       // getenv (OCR 模型目录)
#include <uiautomation.h>   // 无障碍(UI Automation): 外部软件的结构化控件树, 见 layoutTree

// 无障碍要链 uiautomationcore。用 #pragma comment 而不是改 CMake: 这份 .cpp 被**多个 target**
// 各编一份(见各处的 CMakeLists), 逐个加链接容易漏; pragma 写进 .obj, 谁链进来谁带上。
// (MSVC 专有, 但本框架只构建 Windows + MSVC。)
#pragma comment(lib, "uiautomationcore.lib")

// OCR 兜底(JZCv/PP-OCR)是否编入: 构建侧在 HAS_JZCv 时定义 JZ_CU_OCR=1
// (见 binary/code_agent/runtime/CMakeLists.txt)。没编入时下面几个接口一律返回
// "空 / 未找到", 绝不假装成功 —— 退化路径必须显式(见 docs/compute-use-design.md §4.1)。
#ifndef JZ_CU_OCR
#define JZ_CU_OCR 0
#endif
#if JZ_CU_OCR
#include "JZCv/JZPaddleOCR.h"    // PP-OCR (ONNX Runtime, 本地推理)
#include <opencv2/core.hpp>
#endif

// 框架侧(布局通道要用): 语义树 / 活动窗口 / 显示器几何 / 虚拟桌面后端
#include "JZApplication.h"
#include "JZThread.h"       // 虚拟桌面里"进程" = JZThread; 进程名 = objectName (见 createProcess)
#include "JZLog.h"
#include "JZWidget/JZWidget.h"
#include "JZWidget/JZWindow.h"
#include "binary/virtual_device/JZVirtualDevice.h"    // 虚拟桌面 (setVirtualDesktop)
#include "binary/virtual_device/JZVirtualWindow.h"
#include "binary/virtual_device/JZVirtualThread.h"    // jzRunOnThreadSync: 桌面操作要转投主线程
#include <functional>
#include <memory>

// OpenCV 只在"像素族匹配"(matchImage/matchColor)里用，而像素族已降级为**兜底**
// (见 docs/compute-use-design.md §4.1) ⇒ 默认不编，避免为一个兜底能力背整条 OpenCV 依赖。
// 要启用: CMake 里加 `target_compile_definitions(<target> PRIVATE JZ_CU_OPENCV=1)` 并链 OpenCV。
#ifndef JZ_CU_OPENCV
#define JZ_CU_OPENCV 0
#endif
#if JZ_CU_OPENCV
#include <opencv2/opencv.hpp>
#endif

JZVirtualDevice* JZComputerUse::s_virtualDesk = nullptr;

// —— 后端选择 ——
void JZComputerUse::setVirtualDesktop(JZVirtualDevice* desk) {
    s_virtualDesk = desk;
    // 口径 (2026-10-03): 对外一律**物理像素**, 内部恒逻辑 —— click 进来除 scale 再注入,
    // layoutTree 出去乘 scale; 所以这里不再需要"归一化基准"这类共享状态了。
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

// 当前后端的**一帧**(JZImage)。screenshot / matchImage / matchColor / ocrTexts 必须都从
// 这里取 —— 否则坐标会串台(在某个来源上定位, 却按另一个来源的尺寸算)。
// 这一帧的像素空间就是本类对外说的"物理像素" (见头注释"坐标口径")。
//   · 虚拟桌面 => 桌面合成帧(不抓屏, 不受遮挡/z 序/前台/多屏影响);
//   · 真桌面   => 各屏拼接抓屏(captureVirtualDesktop 指**多显示器虚拟屏**, 不是本框架的虚拟桌面)。
// ⚠ 曾经 matchImage/matchColor 直接调 captureVirtualDesktop: 虚拟模式下它们抓的是**本机真屏**,
//   于是桌面上明明摆着目标也永远找不到(2026-09-30 由 test_cu_virtual 的像素族用例暴露)。
JZImage currentFrame() {
    JZVirtualDevice* d = JZComputerUse::virtualDesktop();
    if (d)
        return d->screenshot();
    return JZScreenShot::captureVirtualDesktop(nullptr);
}

// ⚠ 这里曾经有一个 shiftBoundsInScreen(): 给语义树的 boundsInScreen 补上窗口的桌面偏移,
// 因为当时 JZWidget::mapToGlobal 在**虚拟窗口**下退化成"客户区坐标"(它靠 HWND ClientToScreen,
// 虚拟窗口没有原生窗口) ⇒ 只能在外面手动补。JZWindow 抽平台后端之后, 坐标换算归后端
// (虚拟窗口 = 桌面坐标), mapToGlobal 在两个平台都正确 ⇒ 再补一次就是**重复补偿**
// (表现为 layoutTree 给的 rect 整体偏移一个窗口位置, "能定位却点不中")。故删除。

    // （曾经这里有个 tempDir(): 给"把帧存成 PNG 再 imread 读回来"当落盘目录。
    //   matchImage 改成零拷贝包装后它没人用了 —— 见 jzCvMatOf 的说明。）

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

// （曾经这里有个 parseHex(): 把 "#RRGGBB" 转成 OpenCV 的 BGR 标量, 供 matchColor 用。
//   颜色查找改成纯 C++ 实现后它没人用了 —— 见下面 matchColor 的说明。）

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

    // 虚拟桌面: 按名字找那个"进程"(= 一个 JZThread + 一份 JS 运行时)。
    //   ⚠ 名字的来源: JZVirtualProcess 构造时 `setObjectName(name)`（见 JZVirtualProcess.cpp）——
    //     所以 CU 只认 JZThread 这一层就够, 不需要认识 JS 引擎那个类。
    //   带/不带 ".exe" 都认（与真桌面那条路的 pidByName 同一套规则）。
    JZThread* cuVirtualProcessByName(JZVirtualDevice* d, const std::string& name) {
        if (!d || name.empty())
            return nullptr;
        for (JZThread* p : d->processes()) {
            if (!p)
                continue;
            const std::string n = p->objectName();
            if (n == name || n + ".exe" == name)
                return p;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0
                && n == name.substr(0, name.size() - 4))
                return p;
        }
        return nullptr;
    }

    // 虚拟后端: 所有"桌面操作"都必须在**桌面所属线程**（主线程）上做 ——
    //   · 窗口表 / 激活 / 摆位要抢桌面的锁, 而进程线程做这些时主线程可能正卡在 sendEvent 等它(互等);
    //   · terminateProcess 还要 join + 删**另一个线程**, 在错的线程里做会直接踩 JZObject 的线程
    //     亲和断言 —— 实测是 `FATAL JZThread.cpp(422): jzApp->currentThread() == this`, 容器当场被带走。
    // 所以统一转投主线程（jzRunOnThreadSync 自带"已经在本线程就直接跑"）。
    // ⚠ 纪律(见 JZVirtualThread.h): func 里别再回等调用方线程; 捕获用值/shared_ptr(超时后仍可能执行)。
    // ⚠ 别用现成的 jzRunOnThreadSync 来做这件事: 它对目标线程有个 `isRunning()` 前置检查,
    //   而**主线程**的 JZThread 不是 std::thread 起的(没有 joinable 的 thread) ⇒ 那个检查会把
    //   转投整个挡掉, 调用方拿到"什么都没做"(实测: windows() 一律返回空表)。
    //   这里只做"投递 + 信号量等", 形状与它一致; 已经在桌面线程上就直接跑(别自投递)。
    bool cuOnDesktopThread(const std::function<void()>& fn, int timeoutMs = 8000) {
        JZObject* desk = JZComputerUse::virtualDesktop();
        if (!desk)
            return false;
        JZThread* main = jzApp ? jzApp->mainThread() : nullptr;
        // 没有应用 / 没有主线程(如独立的测试 exe, 没有一个在跑事件循环的宿主) ⇒ 没得转投,
        // 就在当前线程上跑: 那种场景下"桌面所属线程"本来就是调用方自己。
        if (!main || JZThread::currentThread() == main) {
            fn();
            return true;
        }
        auto done = std::make_shared<JZSemaphore>();
        const std::function<void()> f = fn;      // 拷一份 (超时后 lambda 仍可能执行)
        runOnObject(desk, [f, done]() {
            try { f(); } catch (...) { }
            done->release();
        });
        return done->tryAcquire(1, timeoutMs);
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

    // 矩形 -> JSON。⚠ 对外口径是**物理像素**, 而语义树给的 boundsInScreen 是**逻辑**坐标,
    // 所以这里要按传入的 scale 换算 (自家窗口走这条; UIA/Win32 本来就是物理 ⇒ scale = 1)。
    Json::Value physRectJson(int x, int y, int w, int h, double scale) {
        if (!(scale > 0.0))
            scale = 1.0;
        Json::Value r(Json::objectValue);
        r["x"] = std::lround((double)x * scale);
        r["y"] = std::lround((double)y * scale);
        r["w"] = std::lround((double)w * scale);
        r["h"] = std::lround((double)h * scale);
        return r;
    }

    // JZWidget 语义树(dumpSemanticTree 的无障碍树) → 本包 LayoutNode 形态。
    // 字段一一对应: className→role / viewId→id / text→text / contentDesc→name /
    //              boundsInScreen→rect(**物理像素**: 框架给的是逻辑坐标, 这里 ×scale) /
    //              visible·enabled·clickable·checked·range·items·actions 直传。
    Json::Value widgetTreeToLayout(const Json::Value& n, double scale) {
        Json::Value o(Json::objectValue);
        o["role"] = n.isMember("className") ? n["className"].asString() : std::string("unknown");
        if (n.isMember("viewId")) o["id"] = n["viewId"];            // = objectName()（路径的稳定片段）
        if (n.isMember("text")) o["text"] = n["text"];
        if (n.isMember("contentDesc")) o["name"] = n["contentDesc"];
        const Json::Value& b = n["boundsInScreen"];
        if (b.isArray() && b.size() >= 4)
            o["rect"] = physRectJson(b[0].asInt(), b[1].asInt(), b[2].asInt(), b[3].asInt(), scale);
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
            for (const auto& c : kids) arr.append(widgetTreeToLayout(c, scale));
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
    // ⚠ GetWindowRect 在 DPI-aware 进程里给的就是**物理**像素 (与对外口径一致) ⇒ scale = 1
    Json::Value hwndTreeToLayout(HWND w) {
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
            o["rect"] = physRectJson(r.left, r.top, r.right - r.left, r.bottom - r.top, 1.0);
        o["visible"]   = IsWindowVisible(w) != 0;
        o["enabled"]   = IsWindowEnabled(w) != 0;
        o["clickable"] = true;

        Json::Value arr(Json::arrayValue);
        for (HWND c = GetWindow(w, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            if (!IsWindowVisible(c)) continue;                    // 不可见的子窗口不进树(与自家语义树一致)
            arr.append(hwndTreeToLayout(c));
        }
        if (!arr.empty()) o["children"] = arr;
        return o;
    }

    // ————————————————————————————————————————————————
    // 无障碍(UI Automation): 外部软件的**结构化控件树**
    // ————————————————————————————————————————————————
    // 为什么要有这条腿: Win32 子窗口枚举只认"标准控件" —— 浏览器 / Electron / WPF / Qt 自绘
    // 这些界面在它眼里常常只有一层空壳; 而 UIA 由**应用自己**暴露语义(名字/角色/值/可操作模式),
    // 是外部软件唯一"不靠猜像素"的结构化来源(见 docs/compute-use-design.md §2/§7)。
    // ⚠ 虚拟桌面下够不到(虚拟窗口没有原生 HWND) ⇒ 明确返回空, 由调用方退到自家语义树。

    const int kUiaMaxDepth = 12;    // 树深上限
    const int kUiaMaxNodes = 400;   // 节点预算上限(UIA 每取一个属性都是一次跨进程 COM 调用)

    // BSTR(UTF-16) -> UTF-8
    std::string bstrToUtf8(BSTR s) {
        if (!s || !*s)
            return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1)
            return std::string();
        std::string out((size_t)(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr);
        return out;
    }

    // UIA ControlTypeId -> 本包词汇(与 roleFromClass / widgetTreeToLayout 用同一套词)
    std::string roleFromControlType(int typeId) {
        switch (typeId) {
        case UIA_ButtonControlTypeId:      return "button";
        case UIA_EditControlTypeId:        return "edit";
        case UIA_TextControlTypeId:        return "text";
        case UIA_DocumentControlTypeId:    return "document";
        case UIA_WindowControlTypeId:      return "window";
        case UIA_PaneControlTypeId:        return "pane";
        case UIA_GroupControlTypeId:       return "group";
        case UIA_ListControlTypeId:        return "list";
        case UIA_ListItemControlTypeId:    return "listitem";
        case UIA_TreeControlTypeId:        return "tree";
        case UIA_TreeItemControlTypeId:    return "treeitem";
        case UIA_MenuControlTypeId:        return "menu";
        case UIA_MenuItemControlTypeId:    return "menuitem";
        case UIA_MenuBarControlTypeId:     return "menubar";
        case UIA_CheckBoxControlTypeId:    return "checkbox";
        case UIA_RadioButtonControlTypeId: return "radiobutton";
        case UIA_ComboBoxControlTypeId:    return "combobox";
        case UIA_TabControlTypeId:         return "tab";
        case UIA_TabItemControlTypeId:     return "tabitem";
        case UIA_ToolBarControlTypeId:     return "toolbar";
        case UIA_StatusBarControlTypeId:   return "statusbar";
        case UIA_ScrollBarControlTypeId:   return "scrollbar";
        case UIA_SliderControlTypeId:      return "slider";
        case UIA_SpinnerControlTypeId:     return "spinbox";
        case UIA_ProgressBarControlTypeId: return "progressbar";
        case UIA_ImageControlTypeId:       return "image";
        case UIA_HyperlinkControlTypeId:   return "hyperlink";
        case UIA_TableControlTypeId:       return "table";
        case UIA_DataItemControlTypeId:    return "dataitem";
        case UIA_HeaderControlTypeId:      return "header";
        case UIA_SeparatorControlTypeId:   return "separator";
        case UIA_ToolTipControlTypeId:     return "tooltip";
        default:                           return "unknown";
        }
    }

    // 一次 COM 初始化的 RAII 守卫: UIA 要 COM(STA)。与 switchToDesktop 同款"随用随初始化",
    // 但这里用 RAII —— 中途 return 也不会漏 CoUninitialize。
    class CuComGuard {
    public:
        CuComGuard() {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            m_ok = SUCCEEDED(hr);
            // 本线程已在别的 apartment(RPC_E_CHANGED_MODE): 照用, 但生命周期归那边管,
            // 这里**不能** CoUninitialize。
            m_owned = (hr == S_OK || hr == S_FALSE);
        }
        ~CuComGuard() { if (m_owned) CoUninitialize(); }
        bool ok() const { return m_ok; }
    private:
        bool m_ok = false;
        bool m_owned = false;
    };

    // UIA 元素 -> LayoutNode。形状与 hwndTreeToLayout 一致(role/id/text/name/rect/visible/
    // enabled/clickable/actions/children), JS 侧那套 layout 寻址一行都不用改。
    // ⚠ UIA 的 BoundingRectangle 就是**物理**屏幕坐标 (DPI-aware 客户端不做虚拟化)
    //   ⇒ scale = 1, 与对外口径天然一致。
    Json::Value uiaElementToLayout(IUIAutomationElement* e, IUIAutomationTreeWalker* walker,
                                   int depth, int* budget) {
        Json::Value o(Json::objectValue);
        if (!e || *budget <= 0)
            return o;
        (*budget)--;

        CONTROLTYPEID typeId = 0;
        if (SUCCEEDED(e->get_CurrentControlType(&typeId)))
            o["role"] = roleFromControlType((int)typeId);

        BSTR b = nullptr;
        if (SUCCEEDED(e->get_CurrentName(&b)) && b && *b) {
            // 自家语义树用 name、Win32 树用 text —— 两个都写, 消费方按自己的惯例取
            o["text"] = bstrToUtf8(b);
            o["name"] = bstrToUtf8(b);
            SysFreeString(b);
            b = nullptr;
        }
        if (SUCCEEDED(e->get_CurrentAutomationId(&b)) && b && *b) {
            o["id"] = bstrToUtf8(b);      // 开发给的稳定标识, 优先当路径片段
            SysFreeString(b);
            b = nullptr;
        }
        if (!o.isMember("id") && SUCCEEDED(e->get_CurrentClassName(&b)) && b && *b) {
            o["id"] = bstrToUtf8(b);      // 没有 AutomationId 就拿类名当片段
            SysFreeString(b);
            b = nullptr;
        }

        RECT r = {};
        if (SUCCEEDED(e->get_CurrentBoundingRectangle(&r)))
            o["rect"] = physRectJson(r.left, r.top, r.right - r.left, r.bottom - r.top, 1.0);

        BOOL off = FALSE, en = FALSE;
        if (SUCCEEDED(e->get_CurrentIsOffscreen(&off)))
            o["visible"] = (off == FALSE);
        if (SUCCEEDED(e->get_CurrentIsEnabled(&en)))
            o["enabled"] = (en != FALSE);

        // 别让调用方猜"这个能不能点": 把可用的模式直接列出来(与自家语义树的 actions 同用途)
        auto hasPattern = [e](PATTERNID id) -> bool {
            IUnknown* u = nullptr;
            if (SUCCEEDED(e->GetCurrentPatternAs(id, IID_IUnknown, (void**)&u)) && u) {
                u->Release();
                return true;
            }
            return false;
        };
        Json::Value acts(Json::arrayValue);
        if (hasPattern(UIA_InvokePatternId))         acts.append("invoke");
        if (hasPattern(UIA_TogglePatternId))         acts.append("toggle");
        if (hasPattern(UIA_SelectionItemPatternId))  acts.append("select");
        if (hasPattern(UIA_ValuePatternId))          acts.append("setValue");
        if (hasPattern(UIA_ExpandCollapsePatternId)) acts.append("expand");
        if (!acts.empty()) {
            o["actions"] = acts;
            o["clickable"] = true;
        } else {
            o["clickable"] = false;
        }

        if (depth < kUiaMaxDepth && *budget > 0) {
            Json::Value kids(Json::arrayValue);
            IUIAutomationElement* c = nullptr;
            if (SUCCEEDED(walker->GetFirstChildElement(e, &c)) && c) {
                while (c && *budget > 0) {
                    BOOL kidOff = FALSE;
                    c->get_CurrentIsOffscreen(&kidOff);
                    if (kidOff == FALSE)      // 与 Win32 树、自家语义树一致: 不可见的不进树
                        kids.append(uiaElementToLayout(c, walker, depth + 1, budget));
                    IUIAutomationElement* next = nullptr;
                    walker->GetNextSiblingElement(c, &next);
                    c->Release();
                    c = next;
                }
            }
            if (!kids.empty())
                o["children"] = kids;
        }
        return o;
    }

    // 外部窗口的无障碍树。拿不到 ⇒ null(调用方退到 Win32 子窗口枚举)。
    Json::Value uiaTreeToLayout(HWND hwnd) {
        if (JZComputerUse::isVirtualDesktop())
            return Json::Value();          // 虚拟桌面: UIA 够不到, 明确空(不假装成功)
        if (!hwnd || !IsWindow(hwnd))
            return Json::Value();

        CuComGuard com;
        if (!com.ok())
            return Json::Value();

        IUIAutomation* uia = nullptr;
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&uia))) || !uia)
            return Json::Value();

        Json::Value tree;
        IUIAutomationElement* root = nullptr;
        if (SUCCEEDED(uia->ElementFromHandle(hwnd, &root)) && root) {
            IUIAutomationTreeWalker* walker = nullptr;
            if (SUCCEEDED(uia->get_ControlViewWalker(&walker)) && walker) {
                int budget = kUiaMaxNodes;
                tree = uiaElementToLayout(root, walker, 0, &budget);
                walker->Release();
            }
            root->Release();
        }
        uia->Release();

        // 只有一层空壳(没有子节点) ⇒ 对"找到界面里的东西"没有增益, 让调用方走 Win32 枚举
        if (tree.isNull() || !tree.isMember("children"))
            return Json::Value();
        return tree;
    }
}

// 物理像素 -> 虚拟屏幕绝对坐标(0..65535, SendInput 用)
//
// ⚠ 输入已经是**物理像素** (与本类对外的口径一致), 而 DPI-aware 进程里
//   GetSystemMetrics(SM_*) 给的就是物理像素 ⇒ 正常情况下直接映射, **不再乘 devicePixelRatio**。
//   唯一要补偿的是"进程不是 DPI-aware": 那时系统把整个进程的坐标虚拟化了, 抓屏/调用方看到的
//   "物理像素"其实被缩过一遍, 得乘回去。判据仍是那个自校准:
//   主屏的 SM_CXSCREEN 比它的逻辑宽大 ⇒ 进程是 DPI-aware。
//   `MOUSEEVENTF_VIRTUALDESK` 让 0..65535 覆盖**整个虚拟桌面**（与 vsX/vsW 同口径），
//   多显示器下不设它会按主屏算，副屏上的坐标就偏了。
void JZComputerUse::toAbsolute(double x, double y, DWORD& ax, DWORD& ay) {
    int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsW = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    int vsH = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));

    // 非 DPI-aware 进程: 它看到的坐标是虚拟化过的 ⇒ 乘回该点所在屏的 devicePixelRatio
    double k = 1.0;
    JZScreen pri = JZScreen::primaryScreen();
    if (pri.isValid()) {
        JZRect pg = pri.geometry();
        const bool dpiAware = (GetSystemMetrics(SM_CXSCREEN) > pg.width() + 1);
        if (!dpiAware) {
            JZScreen sc = JZScreen::screenAt(JZPoint((int)x, (int)y));
            if (sc.isValid() && sc.devicePixelRatio() > 0)
                k = sc.devicePixelRatio();
        }
    }

    double px = x * k, py = y * k;
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
    // 虚拟桌面: 与真桌面那条路**同一套语义** —— 真桌面是 IShellDispatch4::MinimizeAll,
    //   这里就是把桌面上所有窗口最小化 (露出"它自己那块桌面"), 不碰真实系统。
    //   ⚠ 别改成 no-op: 脚本在两个后端要能复用同一句 (这是"虚拟里跑通能上真机"的前提)。
    if (s_virtualDesk) {
        JZVirtualDevice* d = s_virtualDesk;
        cuOnDesktopThread([d]() {
            for (JZVirtualWindow* w : d->windowList()) {
                if (w && w->isVisible() && !w->isMinimized())
                    w->setMinimized(true);
            }
            d->refresh();
        });
        return;
    }
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

bool JZComputerUse::click(double x, double y, MouseBtn btn) {
    // 虚拟桌面: **物理像素 ÷ 桌面 scale = 桌面逻辑坐标** -> 命中窗口(并激活它) ->
    //           换算客户区 -> 投递控件
    if (s_virtualDesk) {
        const int lx = s_virtualDesk->toLogical((int)std::lround(x));
        const int ly = s_virtualDesk->toLogical((int)std::lround(y));
        // 与真桌面那条路 (SendInput) **同一个形状**: 送出去就返回, 由虚拟桌面自己调度 ——
        // 这里不等、也不挂回调。要"动作后的画面"用 waitStable (两个后端同一套, 见它的说明)。
        return s_virtualDesk->injectClickAt(lx, ly, cuVirtualBtn(btn));
    }
    DWORD ax, ay; toAbsolute(x, y, ax, ay);
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

bool JZComputerUse::doubleClick(double x, double y, MouseBtn btn) {
    click(x, y, btn); Sleep(20); click(x, y, btn); return true;
}

bool JZComputerUse::drag(double x1, double y1, double x2, double y2) {
    // 虚拟桌面: 按下 -> 中间位移若干步 (控件靠 move 判拖拽) -> 抬起
    // (四个坐标都是**物理像素** => 先换成桌面逻辑再注入)
    if (s_virtualDesk) {
        JZVirtualDevice* d = s_virtualDesk;
        const int ax = d->toLogical((int)std::lround(x1)), ay = d->toLogical((int)std::lround(y1));
        const int bx = d->toLogical((int)std::lround(x2)), by = d->toLogical((int)std::lround(y2));
        d->injectMouseAtScreen(JZMouseEvent::Press, JZMouseEvent::LeftButton, ax, ay);
        // 抓取由控件接管 (grabMouse/m_mouseGrabber), 后续 move 会直发它 —— 与真实拖拽同一条路
        for (int i = 1; i <= 5; ++i) {
            const int xi = ax + (bx - ax) * i / 5;
            const int yi = ay + (by - ay) * i / 5;
            d->injectMouseAtScreen(JZMouseEvent::Move, JZMouseEvent::LeftButton, xi, yi);
        }
        d->injectMouseAtScreen(JZMouseEvent::Release, JZMouseEvent::LeftButton, bx, by);
        return true;                      // 同上: 送出去就返回 (同真桌面 SendInput)
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
        // ⚠ press 与 release 之间**必须隔开一拍**: 应用是在自己的"逻辑帧"里读"当前按着谁"
        //   (游戏循环/时间线定时器), 而这两个事件会在同一轮事件处理里被一起吃掉 ——
        //   那一帧应用读到的永远是"没按下", 于是按方向键纹丝不动、按空格不开火
        //   (实测: shooter 里脚本对准了也打不出分, 玩家坐标全程没变)。
        //   真桌面那条路本来就是 SendInput(press) -> Sleep(20) -> SendInput(release),
        //   这里与它对齐, 但取**一个逻辑帧(16ms)**而不是 20ms: 这个间隔就是脚本的"按键速率
        //   上限"(一次调用 ≈ 一次按键), 20ms 时只有 50 次/秒, 追不上 60 帧的游戏里"按住"
        //   能跑出来的速度(实测给 shooter 的脚本每帧只挪 6px、横穿屏幕要 4 秒, 而游戏里
        //   按住同一方向 2 秒就够 —— 脚本永远追不上敌机)。
        //   "按下 + 立刻松开"的语义不变: 松开依旧是注入后立刻发生, 只是给应用留出一个
        //   能看见"按下"的逻辑帧。
        Sleep(16);
        s_virtualDesk->injectKey((uint32_t)vk, std::string(), mods, false);
        return;                           // 同上: 送出去就返回 (同真桌面 SendInput)
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
        return;                           // 同上: 送出去就返回 (同真桌面 SendInput)
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
    // 一帧 dataURL(PNG): 尺寸 = 该后端的**物理像素** —— 与 click/match 的坐标同一个空间,
    // 从图里量出来的像素可以直接拿去点 (虚拟桌面 = 桌面逻辑 × scale; 真桌面 = 抓屏)。
    JZImage img = currentFrame();
    return JZScreenShot::toDataUrl(img);
}

#if JZ_CU_OPENCV || JZ_CU_OCR
namespace {

// JZImage(RGBA8888) -> cv::Mat(8UC4) —— **零拷贝**: 直接包住它的像素缓冲。
// ⚠ 只在 img 活着时有效(cv::Mat 持的是裸指针, 不做引用计数); 要留住就自己 clone 一份。
//   行距按 4 字节/像素紧密排列(JZImageData 的布局)。
// 它替掉了原先"把整帧存成 PNG 落盘、再 cv::imread 读回来"的做法 —— 那又慢又脏: 每帧几 MB
// 写盘 + PNG 编解码, 而帧本来就在内存里。
cv::Mat jzCvMatOf(const JZImage& img)
{
    if (img.isNull() || !img.data())
        return cv::Mat();
    const uint8_t* px = img.data()->pixels();
    if (!px)
        return cv::Mat();
    return cv::Mat(img.height(), img.width(), CV_8UC4, (void*)px);
}

} // namespace
#endif

CuRect JZComputerUse::matchImage(const std::string& templatePath, const CuRect& region) {
#if !JZ_CU_OPENCV
    (void)templatePath; (void)region;
    return CuRect();        // OpenCV 未接入(像素族已降级为兜底, 见文件头) ⇒ 视为未找到
#else
    JZImage img = currentFrame();
    // 帧: **零拷贝**包成 cv::Mat(不再落盘 PNG + imread); 模板: 它是输入, 照样从文件读
    cv::Mat hay = jzCvMatOf(img);
    cv::Mat needle = cv::imread(templatePath, cv::IMREAD_COLOR);
    if (hay.empty() || needle.empty()) return CuRect();
    // 通道对齐: 帧是 RGBA8888 ⇒ 把模板也转成 RGBA 再比(模板小, 转一次很便宜)。
    // ⚠ 别反过来把**帧**转成 BGR: 那要拷整帧(大头在这儿), 而这里只需要拷模板。
    cv::cvtColor(needle, needle, cv::COLOR_BGR2RGBA);

    int hw = hay.cols, hh = hay.rows;
    cv::Rect roi(0, 0, hw, hh);
    if (region.w > 0 && region.h > 0) {
        // region 是**物理像素** (与帧同一空间) ⇒ 直接当 roi, 不再乘帧尺寸
        int x0 = std::max(0, (int)region.x);
        int y0 = std::max(0, (int)region.y);
        int ww = std::min(hw - x0, (int)region.w);
        int hh2 = std::min(hh - y0, (int)region.h);
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

    // 与 matchColor / matchText / ocrTexts / layoutTree 同一口径: x,y = 命中框**左上角**,
    // w,h = 框尺寸, **单位物理像素** (帧像素就是物理像素)。
    // ⚠ 这里曾经返回的是"命中**中心** + 模板尺寸" ⇒ 调用方按 `x + w/2` 反算落点就会偏掉半个模板
    //   ("能定位点不中"), 按 rect 去认领布局节点也会认到错的那个。2026-09-30 由 test_cu_virtual
    //   的像素族用例(拿 matchImage 与 matchColor 的同一目标互校)暴露。
    CuRect r;
    r.x = (double)(roi.x + maxLoc.x);
    r.y = (double)(roi.y + maxLoc.y);
    r.w = (double)needle.cols; r.h = (double)needle.rows;
    return r;
#endif
}

namespace {

// "#RRGGBB" / "#RGB" -> 0..255（手写解析: 不为一个兜底函数再拉 include）
bool cuParseHexRgb(const std::string& hex, int* r, int* g, int* b) {
    std::string s = hex;
    if (!s.empty() && s[0] == '#')
        s = s.substr(1);
    if (s.size() == 3)
        s = std::string() + s[0] + s[0] + s[1] + s[1] + s[2] + s[2];
    if (s.size() != 6)
        return false;
    int v[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 2; k++) {
            const char c = s[i * 2 + k];
            int n = -1;
            if (c >= '0' && c <= '9')      n = c - '0';
            else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
            if (n < 0)
                return false;
            v[i] = v[i] * 16 + n;
        }
    }
    *r = v[0]; *g = v[1]; *b = v[2];
    return true;
}

} // namespace

CuRect JZComputerUse::matchColor(const std::string& hexColor, const CuRect& region) {
    // ---- 颜色查找: 单一实现, **不依赖 OpenCV**（有没有 OpenCV 都走这条）----
    // 为什么不用 OpenCV: ① 它就是一次像素扫描, 而那边每次要先把整帧**存成 PNG 落盘**再
    //   `cv::imread` 读回来(为了拿 cv::Mat), 又慢又脏; ② 取框口径也不同 —— 那边把所有命中点
    //   并成**一个**外接矩形, 画面上有若干同色目标(红球/图标)时会被连成一片、中心落在目标
    //   之间, 而"找画面上那个红点、点它"要的正是其中一个 ⇒ 这里取**最大连通块**的框;
    //   ③ 于是容器版与桌面版行为一致, 不会出现"同名 API 两套结果"。
    // 容差 60 = 三通道绝对差之和(每通道平均 ~20, 与原 OpenCV 版同一口径);
    // 返回 (左上角 + 尺寸, 单位**物理像素**) 与 matchImage 同口径。
    JZImage img = currentFrame();
    std::shared_ptr<JZImageData> data = img.data();
    if (!data || data->width() <= 0 || data->height() <= 0 || !data->pixels())
        return CuRect();

    int tr = 0, tg = 0, tb = 0;
    if (!cuParseHexRgb(hexColor, &tr, &tg, &tb))
        return CuRect();

    const int hw = data->width(), hh = data->height();
    const uint8_t* px = data->pixels();
    int x0 = 0, y0 = 0, x1 = hw, y1 = hh;      // region(**物理像素**) -> 像素窗口; 缺省整帧
    if (region.w > 0 && region.h > 0) {
        x0 = std::max(0, (int)region.x);
        y0 = std::max(0, (int)region.y);
        x1 = std::min(hw, x0 + std::max(1, (int)region.w));
        y1 = std::min(hh, y0 + std::max(1, (int)region.h));
        if (x1 <= x0 || y1 <= y0)
            return CuRect();
    }

    const int tol = 60;                        // 与 OpenCV 版同一容差
    std::vector<uint8_t> mask((size_t)hw * (size_t)hh, 0);
    for (int y = y0; y < y1; y++) {
        const uint8_t* row = px + (size_t)y * (size_t)hw * 4;
        for (int x = x0; x < x1; x++) {
            const uint8_t* q = row + (size_t)x * 4;
            const int d = std::abs((int)q[0] - tr) + std::abs((int)q[1] - tg)
                        + std::abs((int)q[2] - tb);
            if (d <= tol)
                mask[(size_t)y * (size_t)hw + (size_t)x] = 1;
        }
    }

    // 连通块(4 邻接)取最大 —— 只要外接矩形, 不记形状
    std::vector<uint8_t> seen((size_t)hw * (size_t)hh, 0);
    std::vector<size_t> stack;
    int bestArea = 0, bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            const size_t i0 = (size_t)y * (size_t)hw + (size_t)x;
            if (!mask[i0] || seen[i0])
                continue;
            int area = 0, mx0 = x, my0 = y, mx1 = x, my1 = y;
            seen[i0] = 1;
            stack.clear();
            stack.push_back(i0);
            while (!stack.empty()) {
                const size_t i = stack.back();
                stack.pop_back();
                const int cx = (int)(i % (size_t)hw), cy = (int)(i / (size_t)hw);
                area++;
                if (cx < mx0) mx0 = cx;
                if (cx > mx1) mx1 = cx;
                if (cy < my0) my0 = cy;
                if (cy > my1) my1 = cy;
                const int nb[4][2] = { { cx - 1, cy }, { cx + 1, cy }, { cx, cy - 1 }, { cx, cy + 1 } };
                for (int k = 0; k < 4; k++) {
                    const int nx = nb[k][0], ny = nb[k][1];
                    if (nx < x0 || nx >= x1 || ny < y0 || ny >= y1)
                        continue;
                    const size_t ni = (size_t)ny * (size_t)hw + (size_t)nx;
                    if (mask[ni] && !seen[ni]) {
                        seen[ni] = 1;
                        stack.push_back(ni);
                    }
                }
            }
            if (area > bestArea) {
                bestArea = area;
                bx0 = mx0; by0 = my0; bx1 = mx1; by1 = my1;
            }
        }
    }
    if (bestArea <= 0)
        return CuRect();

    CuRect r;
    r.x = (double)bx0; r.y = (double)by0;          // **物理像素** (帧像素就是物理像素)
    r.w = (double)(bx1 - bx0 + 1); r.h = (double)(by1 - by0 + 1);
    return r;
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

        // 语义树走窗口的**客户区** (客户区那个真 JZWindow 才是控件树的根:
        // 外框/标题栏是窗口管理器的, 不属于任何控件)。
        Json::Value tree = w->client()->dumpSemanticTree(-1);
        // boundsInScreen 已是**桌面逻辑坐标** (dumpSemanticTree 走 mapToGlobal, 虚拟窗口的
        // mapToGlobal = 客户区坐标 + 窗口客户区在桌面上的原点, 见 JZVirtualWindow::clientToScreen)
        // ⇒ 这里**不再**补窗口偏移 (补了就是重复补偿, rect 会整体偏一个窗口位置);
        //   只按桌面的 scale 换成**物理像素** (对外口径)。
        Json::Value out(Json::objectValue);
        out["source"] = "semantic";
        out["tree"] = widgetTreeToLayout(tree, (double)d->scale());
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

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) {
        // 自家界面: 走框架语义树(带 viewId ⇒ 布局路径稳定, 这才是"运行期应该走的那条路")
        JZApplication* app = JZApplication::instance();
        JZWindow* w = app ? app->activeWindow() : nullptr;
        if (w) {
            // 语义树是**逻辑**坐标 ⇒ 按这个窗口的设备缩放换成物理 (对外口径)
            const double sc = (w->device() && w->device()->scale() > 0.0f)
                                  ? (double)w->device()->scale() : 1.0;
            Json::Value out(Json::objectValue);
            out["source"] = "semantic";
            out["tree"] = widgetTreeToLayout(w->dumpSemanticTree(-1), sc);
            return out;
        }
        // 前台是本进程的窗口但框架拿不到活动窗口(极少见) ⇒ 退化到 Win32 枚举, 不假装成功
    }

    // 外部软件: 先试**无障碍(UI Automation)** —— 浏览器 / Electron / WPF 这类自绘界面里有什么,
    // 只有 UIA 说得清(它们对 Win32 子窗口枚举就是一层空壳); 拿不到(COM 失败 / 目标没实现 UIA /
    // 只有空壳)才退回递归枚举 Win32 子窗口。两条路都**标明来源**(source='uia' | 'win32'),
    // 让调用方知道这次定位是哪来的; 两条都给不出东西 ⇒ 整体返回 null, 由 JS 侧标 degraded
    // 退到像素族(见 docs 的"退化必须显式")。
    Json::Value uiaTree = uiaTreeToLayout(fg);
    if (!uiaTree.isNull()) {
        Json::Value out(Json::objectValue);
        out["source"] = "uia";
        out["tree"] = uiaTree;
        return out;
    }

    Json::Value out(Json::objectValue);
    out["source"] = "win32";
    out["tree"] = hwndTreeToLayout(fg);
    return out;
}

// 尺寸/缩放: "图上的像素 ↔ 物理像素"换算的唯一依据 (见头文件)
Json::Value JZComputerUse::screenInfo()
{
    auto two = [](double a, double b) -> Json::Value {
        Json::Value v(Json::arrayValue);
        v.append(a);
        v.append(b);
        return v;
    };
    Json::Value o(Json::objectValue);
    o["virtual"] = isVirtualDesktop();
    if (s_virtualDesk) {
        JZVirtualDevice* d = s_virtualDesk;
        o["logical"] = two(d->desktopWidth(), d->desktopHeight());
        o["physical"] = two(d->physicalWidth(), d->physicalHeight());
        o["scale"] = (double)d->scale();          // 桌面逻辑 -> 物理 (默认 = 宿主 DPI 缩放)
        o["dpi"] = (int)std::lround(96.0 * (double)d->scale());
        o["explicit"] = d->isScaleExplicit();
        return o;
    }
    // 真桌面: 逻辑 = 逻辑虚拟屏并集 (JZScreen::geometry), 物理 = Win32 的物理虚拟屏并集。
    // ⚠ 进程不是 DPI-aware 时 GetSystemMetrics 给的是被系统虚拟化过的值 ⇒ scale 会 ≈1
    //   (与"调用方看到的世界"一致; toAbsolute 里那次补偿用的就是同一条判据)。
    const JZRect lg = virtualLogicalRect();
    const int pw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int ph = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    o["logical"] = two(lg.width(), lg.height());
    o["physical"] = two(pw, ph);
    const double sc = (lg.height() > 0) ? (double)ph / (double)lg.height() : 1.0;
    o["scale"] = sc;
    o["dpi"] = (int)std::lround(96.0 * sc);
    o["explicit"] = false;
    return o;
}

// ————————————————————————————————————————————————
// OCR 兜底（像素族里的"文字"那一条；对应 JS 侧 Locator{kind:'ocr', text}）
// ————————————————————————————————————————————————
// 定位: 本地 PP-OCR(JZCv/JZPaddleOCR) —— 不依赖外部服务; 虚拟桌面下直接读桌面合成帧。
// 这是**显式退化**路径: 没编入/没模型/没识别到都返回"空/未找到", 不假装成功。
#if JZ_CU_OCR
namespace {

JZPaddleOCR* g_ocr = nullptr;   // 进程内共享: 模型加载慢, 别每次重建
std::string  g_ocrRoot;         // 实际用上的模型目录(给 ocrInfo 看: "到底加载的哪份模型")

bool jzCvFileExists(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

// JZImage(RGBA8888) -> cv::Mat(8UC4)。JZPaddleOCR 内部会把 RGBA 转成 BGR。
// ⚠ 这里 **clone 一份**: OCR 的调用方不保证在帧的生存期内用完(见 jzCvMatOf 的说明);
//   像素布局的换算只写一份, 复用上面那个零拷贝包装。
cv::Mat jzCvImageToMat(const JZImage& img)
{
    return jzCvMatOf(img).clone();
}

// region(**物理像素**, 相对整屏) -> 像素裁剪矩形, 并夹到图内 (region 为空 => 整屏)
// 帧像素就是物理像素 ⇒ 直接用, 不再乘帧尺寸。
cv::Rect jzCvRoiOf(const CuRect& region, int w, int h)
{
    if (region.w <= 0 || region.h <= 0)
        return cv::Rect(0, 0, w, h);
    int x0 = (int)std::lround(region.x);
    int y0 = (int)std::lround(region.y);
    int x1 = (int)std::lround(region.x + region.w);
    int y1 = (int)std::lround(region.y + region.h);
    x0 = std::max(0, std::min(x0, w - 1));
    y0 = std::max(0, std::min(y0, h - 1));
    x1 = std::max(x0 + 1, std::min(x1, w));
    y1 = std::max(y0 + 1, std::min(y1, h));
    return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

std::string jzCvLower(std::string s)
{
    for (auto& c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

} // namespace
#endif   // JZ_CU_OCR

bool JZComputerUse::initOcr(const std::string& modelRoot)
{
#if !JZ_CU_OCR
    (void)modelRoot;
    LOG_FMT_E("[CU] OCR 未编入本构建 (需要 JZCv 模块: OpenCV + ONNX Runtime)");
    return false;
#else
    if (g_ocr && g_ocr->isInit())
        return true;

    // 模型目录: 显式传 > 环境变量 JZ_CU_OCR_MODEL > <exe目录>/model/paddle_ocr
    std::string root = modelRoot;
    if (root.empty()) {
        if (const char* env = getenv("JZ_CU_OCR_MODEL"))
            root = env;
    }
    if (root.empty()) {
        char exe[MAX_PATH] = {};
        if (::GetModuleFileNameA(nullptr, exe, MAX_PATH) > 0) {
            std::string p = exe;
            const size_t cut = p.find_last_of("\\/");
            if (cut != std::string::npos)
                root = p.substr(0, cut) + "/model/paddle_ocr";
        }
    }
    if (root.empty()) {
        LOG_FMT_E("[CU] OCR 模型目录未指定 (传参 / JZ_CU_OCR_MODEL)");
        return false;
    }
    const char last = root[root.size() - 1];
    const std::string base = root + ((last == '/' || last == '\\') ? "" : "/");

    delete g_ocr;
    g_ocr = new JZPaddleOCR();

    // 两套布局都认: ① 开发布局(与 work/lib/PaddleOCR/model 一致) ② 平铺布局
    bool ok = false;
    if (jzCvFileExists(base + "PP-OCRv6_small_det/inference.onnx"))
        ok = g_ocr->initFromDir(root);
    if (!ok)
        ok = g_ocr->init(base + "det.onnx", base + "rec.onnx", base + "dict.txt");
    if (!ok) {
        LOG_FMT_E("[CU] OCR 模型载入失败: %s", base.c_str());
        return false;
    }
    g_ocrRoot = root;                   // 记下实际用上的目录(ocrInfo 会报给外面)
    LOG_FMT_I("[CU] OCR 就绪 (cls=%d): %s", (int)g_ocr->hasClassifier(), base.c_str());
    return true;
#endif
}

bool JZComputerUse::ocrReady()
{
#if !JZ_CU_OCR
    return false;
#else
    return g_ocr && g_ocr->isInit();
#endif
}

void JZComputerUse::deinitOcr()
{
#if JZ_CU_OCR
    delete g_ocr;
    g_ocr = nullptr;
    g_ocrRoot.clear();
#endif
}

bool JZComputerUse::ocrBuilt()
{
#if !JZ_CU_OCR
    return false;
#else
    return true;
#endif
}

Json::Value JZComputerUse::ocrInfo()
{
    Json::Value v;
    v["built"] = ocrBuilt();
#if !JZ_CU_OCR
    v["ready"] = false;
    v["cls"] = false;
    v["modelRoot"] = "";
    v["hint"] = "本构建未编入 OCR(JZ_CU_OCR): 需要 JZCv 模块(OpenCV + ONNX Runtime)";
#else
    v["ready"] = ocrReady();
    v["cls"] = (g_ocr && g_ocr->isInit()) ? g_ocr->hasClassifier() : false;
    v["modelRoot"] = g_ocrRoot;
#endif
    return v;
}

Json::Value JZComputerUse::ocrTexts(const CuRect& region, double minScore)
{
    Json::Value arr(Json::arrayValue);
#if !JZ_CU_OCR
    (void)region; (void)minScore;
    return arr;
#else
    if (!ocrReady())
        return arr;

    // 取一帧 (与 screenshot/match 同源): 虚拟桌面走合成帧, 真桌面走抓屏
    JZImage img = currentFrame();
    if (img.isNull())
        return arr;

    const cv::Mat mat = jzCvImageToMat(img);
    if (mat.empty())
        return arr;

    const cv::Rect roi = jzCvRoiOf(region, mat.cols, mat.rows);
    cv::Mat part = mat(roi);        // 只 OCR 这个小区域: 省时间, 也少误检

    const std::vector<JZOCRResult> res = g_ocr->ocr(part, (float)minScore);
    for (const auto& r : res) {
        Json::Value o(Json::objectValue);
        o["text"]  = r.text;
        o["score"] = r.score;
        // ROI 内像素 + ROI 原点 = 整帧**物理像素** (与 layoutTree/click 同口径 => 直接能点)
        o["x"] = (double)(roi.x + r.rect.x);
        o["y"] = (double)(roi.y + r.rect.y);
        o["w"] = (double)r.rect.width;
        o["h"] = (double)r.rect.height;
        arr.append(o);
    }
    return arr;
#endif
}

CuRect JZComputerUse::matchText(const std::string& text, const CuRect& region, double minScore)
{
#if !JZ_CU_OCR
    (void)text; (void)region; (void)minScore;
    return CuRect();
#else
    CuRect miss;    // w==0 = 未找到
    if (text.empty() || !ocrReady())
        return miss;

    const Json::Value all = ocrTexts(region, minScore);
    const std::string needle = jzCvLower(text);

    const Json::Value* best = nullptr;
    double bestScore = -1;
    bool bestExact = false;
    for (const auto& o : all) {
        const std::string got = jzCvLower(o["text"].asString());
        if (got.empty())
            continue;
        const bool exact = (got == needle);
        if (!exact && got.find(needle) == std::string::npos)
            continue;
        const double sc = o["score"].asDouble();
        // 优先"完全相等", 其次置信度更高 (det 已按阅读顺序给出, 同分保持更早的)
        if (!best || (exact && !bestExact) || (exact == bestExact && sc > bestScore)) {
            best = &o;
            bestScore = sc;
            bestExact = exact;
        }
    }
    if (!best)
        return miss;

    CuRect hit;
    hit.x = (*best)["x"].asDouble();
    hit.y = (*best)["y"].asDouble();
    hit.w = (*best)["w"].asDouble();
    hit.h = (*best)["h"].asDouble();
    return hit;
#endif
}

bool JZComputerUse::waitStable(int settleMs, int timeoutMs) {
    // ⚠ 两个后端**同一套**: 输入都是"送出去就返回、由后端自己调度"(真桌面 SendInput / 虚拟桌面
    //   投递 —— 见 JZVirtualWindow::injectMouse), 所以"动作生效了没"只能靠**看画面**判断:
    //   连续两帧(缩略图)相同并保持 settleMs 就算稳。
    //   (原来虚拟桌面这里直接 return true —— 那是"输入同步注入"年代的遗留; 输入改成投递之后
    //    它就成了假话, 而且脚本在两个后端行为不一致。要做"真桌面也能跑"的脚本, 语义就得一样。)
    if (settleMs < 0) settleMs = 300;
    if (timeoutMs <= 0) timeoutMs = 10000;
    const int step = 100;

    // 像素在这里**只当信号**: 连续两帧"缩略图相同"就认为画面没在变。
    // 比对用缩略图的 PNG 字节(不引图像比较 API, 也不用 OpenCV)。
    auto fingerprint = [](std::string& out) -> bool {
        JZImage img = currentFrame();         // 帧来源按后端取: 虚拟桌面 = 桌面合成帧, 真桌面 = 抓屏
        if (img.isNull()) return false;
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
    // 虚拟桌面: 不做"按 pid 杀" —— 虚拟进程没有系统 pid（JZVirtualProcess::processId 只是它
    //   自己的序号, CU 这一层认不到那个类）。按**名字**停请走 terminateProcess(name)。
    //   这里明确失败, 不假装成功。
    if (s_virtualDesk)
        return false;
    HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return false;
    bool ok = TerminateProcess(h, 0) != 0;
    CloseHandle(h);
    return ok;
}

bool JZComputerUse::terminateProcess(const std::string& name) {
    // 虚拟桌面: 停掉桌面上那个进程 (= 一个线程 + 一份 JS 运行时; 它建的窗口一起收走)。
    //   ⚠ 与真桌面那条路**同一套语义**（按名字停), 只是实现落在 JZVirtualDevice::terminateProcess。
    if (s_virtualDesk) {
        JZVirtualDevice* d = s_virtualDesk;
        // ⚠ "停自己"必须拦掉: 转投主线程后主线程会 join 当前这个线程, 而它正阻塞等这次 RPC 返回 ⇒ 死锁
        if (JZThread* self = cuVirtualProcessByName(d, name)) {
            if (self == JZThread::currentThread())
                return false;
        }
        auto ok = std::make_shared<bool>(false);
        const bool ran = cuOnDesktopThread([d, name, ok]() {
            JZThread* p = cuVirtualProcessByName(d, name);
            if (!p)
                return;
            d->terminateProcess(p);                 // 停 + 等 + 收它的窗口 + 删
            *ok = true;
        }, 20000);                                  // 它要 join 目标线程, 给足时间
        return ran && *ok;
    }
    DWORD pid = pidByName(name);
    if (!pid) return false;
    return terminateProcess(pid);
}

bool JZComputerUse::focusProcess(DWORD pid) {
    // 虚拟桌面: 没有"系统 pid"这一层 —— 按名字切请走 focusProcess(name) / focusWindow(title)。
    //   这里明确失败, 不假装成功。
    if (s_virtualDesk)
        return false;
    FindCtx c; c.pid = pid;
    EnumWindows(EnumFindPid, (LPARAM)&c);
    if (!c.found) return false;
    bringToFront(c.found);
    return true;
}

bool JZComputerUse::focusProcess(const std::string& name) {
    // 虚拟桌面: 激活那个进程的窗口（z 序最上那个 = "它的主窗口"）—— 与真桌面那条路同一套语义:
    //   最小化先还原; 返回值 = "它现在真是活动窗口吗"（不是"找到了"）。
    if (s_virtualDesk) {
        JZVirtualDevice* d = s_virtualDesk;
        auto ok = std::make_shared<bool>(false);
        const bool ran = cuOnDesktopThread([d, name, ok]() {
            JZThread* p = cuVirtualProcessByName(d, name);
            if (!p)
                return;
            const std::vector<JZVirtualWindow*> wins = d->windowsOfProcess(p);
            if (wins.empty())
                return;
            JZVirtualWindow* w = wins.back();          // z 序末尾最上
            if (w->isMinimized())
                w->setMinimized(false);
            d->activate(w);
            d->refresh();
            *ok = (d->activeWindow() == w);
        });
        return ran && *ok;
    }
    DWORD pid = pidByName(name);
    if (!pid) return false;
    return focusProcess(pid);
}

bool JZComputerUse::focusWindow(const std::string& title) {
    // 虚拟桌面: 激活桌面上标题匹配的那个窗口 —— 与真桌面那条路**同一套语义**:
    //   · 最小化先还原 (真桌面 bringToFront 里的 ShowWindow(SW_RESTORE) 干的就是这件事);
    //   · 返回值 = "它现在真是活动窗口吗" (真桌面返回的是"真成了前台没有"), 而不是"找到了"。
    if (s_virtualDesk) {
        JZVirtualDevice* d = s_virtualDesk;
        auto ok = std::make_shared<bool>(false);
        const bool ran = cuOnDesktopThread([d, title, ok]() {
            JZVirtualWindow* w = d->byTitle(title);
            if (!w)
                return;
            if (w->isMinimized())
                w->setMinimized(false);
            d->activate(w);
            d->refresh();
            *ok = (d->activeWindow() == w);
        });
        return ran && *ok;
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

// ————————————————————————————————————————————————
// 窗口表 / 窗口状态（"EnumWindows / SetWindowPos / ShowWindow / SetWindowText" 那一组）
// 两个后端同一套语义、同一套坐标口径（**物理像素**）—— 见头文件的说明。
// ————————————————————————————————————————————————
namespace {

// 窗口句柄字符串: 与 JZVirtualDeviceHttp::windowHandleOf **同一套写法**（"指针的十六进制"）。
// 于是同一台桌面上, CU 与 HTTP 报出来的句柄是同一个字符串, 两边对照调试不会打架。
// 对调用方是**不透明**的: 只用来原样带回来。
std::string cuHandleStr(const void* p) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)(uintptr_t)p);
    return std::string(buf);
}

Json::Value cuRectArr(int x, int y, int w, int h) {
    Json::Value v(Json::arrayValue);
    v.append(x); v.append(y); v.append(w); v.append(h);
    return v;
}

// 虚拟桌面: 窗口 -> 对外 JSON（乘 scale: 对外一律物理像素, 与 click/screenshot 同空间）
Json::Value cuVirtualWindowJson(JZVirtualDevice* d, JZVirtualWindow* w, int index) {
    Json::Value o(Json::objectValue);
    o["handle"] = cuHandleStr(w);
    o["index"] = index;
    o["title"] = w->title();
    const JZRect f = w->frameGeometry();
    const JZRect c = w->clientGeometry();
    o["frame"] = cuRectArr(d->toPhysical(f.left()), d->toPhysical(f.top()),
                           d->toPhysical(f.width()), d->toPhysical(f.height()));
    o["client"] = cuRectArr(d->toPhysical(c.left()), d->toPhysical(c.top()),
                            d->toPhysical(c.width()), d->toPhysical(c.height()));
    o["visible"] = w->isVisible();
    o["active"] = (w == d->activeWindow());
    o["minimized"] = w->isMinimized();
    o["maximized"] = w->isMaximized();
    return o;
}

// 下标写法: "#3" / "3" -> 3（不是纯数字返回 -1; 句柄/标题走别的分支）
int cuParseIndex(const std::string& key) {
    std::string n = key;
    if (!n.empty() && n[0] == '#')
        n = n.substr(1);
    if (n.empty())
        return -1;
    for (char ch : n)
        if (!isdigit((unsigned char)ch))
            return -1;
    return atoi(n.c_str());
}

// 虚拟桌面: 窗口标识 -> 窗口（与 HTTP 的 findWindow 同一套认法: 句柄 > 下标 > 标题片段[最上层优先]）
JZVirtualWindow* cuVirtualFind(JZVirtualDevice* d, const std::string& key) {
    if (key.empty())
        return nullptr;
    const std::vector<JZVirtualWindow*> list = d->windowList();
    if (key.size() > 2 && key[0] == '0' && (key[1] == 'x' || key[1] == 'X')) {
        for (JZVirtualWindow* w : list)
            if (w && cuHandleStr(w) == key)
                return w;
        return nullptr;
    }
    const int idx = cuParseIndex(key);
    if (idx >= 0)
        return (idx < (int)list.size()) ? list[idx] : nullptr;
    for (size_t i = list.size(); i-- > 0;)          // z 序末尾最上 ⇒ 从后往前 = 最上层优先
        if (list[i] && list[i]->title().find(key) != std::string::npos)
            return list[i];
    return nullptr;
}

// ---- 真桌面那一套 ----
BOOL CALLBACK cuEnumVisibleTop(HWND w, LPARAM l) {
    auto* out = (std::vector<HWND>*)l;
    if (!IsWindowVisible(w))
        return TRUE;
    if (GetWindow(w, GW_OWNER) != nullptr)          // 只要顶层窗口（跳过被拥有的工具窗）
        return TRUE;
    out->push_back(w);
    return TRUE;
}

std::vector<HWND> cuTopWindows() {
    std::vector<HWND> out;
    EnumWindows(cuEnumVisibleTop, (LPARAM)&out);
    return out;
}

std::string cuWinTitle(HWND w) {
    wchar_t buf[512] = {};
    GetWindowTextW(w, buf, (int)(sizeof(buf) / sizeof(buf[0])));
    return narrow(buf);                             // UTF-8（与进程名同一个口径）
}

Json::Value cuWin32WindowJson(HWND w, int index) {
    Json::Value o(Json::objectValue);
    o["handle"] = cuHandleStr(w);
    o["index"] = index;
    o["title"] = cuWinTitle(w);
    RECT fr = {};
    GetWindowRect(w, &fr);
    o["frame"] = cuRectArr((int)fr.left, (int)fr.top,
                           (int)(fr.right - fr.left), (int)(fr.bottom - fr.top));
    RECT cr = {};
    GetClientRect(w, &cr);
    POINT tl = { 0, 0 };
    ClientToScreen(w, &tl);
    o["client"] = cuRectArr((int)tl.x, (int)tl.y,
                            (int)(cr.right - cr.left), (int)(cr.bottom - cr.top));
    o["visible"] = IsWindowVisible(w) != 0;
    o["active"] = (w == GetForegroundWindow());
    o["minimized"] = IsIconic(w) != 0;
    o["maximized"] = IsZoomed(w) != 0;
    return o;
}

HWND cuWin32Find(const std::string& key) {
    if (key.empty())
        return nullptr;
    if (key.size() > 2 && key[0] == '0' && (key[1] == 'x' || key[1] == 'X'))
        return (HWND)(uintptr_t)strtoull(key.c_str(), nullptr, 16);
    const std::vector<HWND> list = cuTopWindows();
    const int idx = cuParseIndex(key);
    if (idx >= 0)
        return (idx < (int)list.size()) ? list[idx] : nullptr;
    for (HWND w : list)
        if (cuWinTitle(w).find(key) != std::string::npos)
            return w;
    return nullptr;
}

int cuWin32IndexOf(HWND h) {
    const std::vector<HWND> list = cuTopWindows();
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i] == h)
            return (int)i;
    return -1;
}

// UTF-8 -> 宽字符（SetWindowTextW 用）
std::wstring cuWide(const std::string& s) {
    if (s.empty())
        return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

} // namespace

Json::Value JZComputerUse::windows(const std::string& optsJson) {
    std::string titleFilter;
    if (!optsJson.empty()) {
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(optsJson, root) && root.isMember("title"))
            titleFilter = root["title"].asString();
    }

    Json::Value arr(Json::arrayValue);
    if (s_virtualDesk) {
        JZVirtualDevice* d = s_virtualDesk;
        auto out = std::make_shared<Json::Value>(Json::arrayValue);
        const bool ran = cuOnDesktopThread([d, titleFilter, out]() {
            const std::vector<JZVirtualWindow*> list = d->windowList();
            for (size_t i = 0; i < list.size(); ++i) {
                JZVirtualWindow* w = list[i];
                if (!w)
                    continue;
                if (!titleFilter.empty() && w->title().find(titleFilter) == std::string::npos)
                    continue;
                out->append(cuVirtualWindowJson(d, w, (int)i));
            }
        });
        return ran ? *out : Json::Value(Json::nullValue);   // 超时给 null(不是空表): 调用方别把"没拿到"当成"没有窗口"
    }
    const std::vector<HWND> list = cuTopWindows();
    for (size_t i = 0; i < list.size(); ++i) {
        if (!titleFilter.empty() && cuWinTitle(list[i]).find(titleFilter) == std::string::npos)
            continue;
        arr.append(cuWin32WindowJson(list[i], (int)i));
    }
    return arr;
}

Json::Value JZComputerUse::windowState(const std::string& optsJson) {
    Json::Value root;
    Json::Reader reader;
    if (optsJson.empty() || !reader.parse(optsJson, root) || !root.isObject())
        return Json::Value();

    const std::string key = root.isMember("key") ? root["key"].asString() : "";
    if (key.empty())
        return Json::Value();
    const std::string action = root.isMember("action") ? root["action"].asString() : "";
    const bool hasPos = root.isMember("x") && root.isMember("y");
    const bool hasSize = root.isMember("w") && root.isMember("h");
    const bool hasTitle = root.isMember("title");
    const double px = hasPos ? root["x"].asDouble() : 0.0;
    const double py = hasPos ? root["y"].asDouble() : 0.0;
    const double pw = hasSize ? root["w"].asDouble() : 0.0;
    const double ph = hasSize ? root["h"].asDouble() : 0.0;
    if (hasSize && (pw <= 0 || ph <= 0))
        return Json::Value();                       // w/h = **客户区**尺寸, 必须 > 0
    static const char* kActions[] = { "", "activate", "minimize", "maximize", "restore", "close" };
    bool known = false;
    for (const char* a : kActions)
        if (action == a) { known = true; break; }
    if (!known)
        return Json::Value();                       // 不认识的 action: 不假装成功

    if (s_virtualDesk) {
        JZVirtualDevice* d = s_virtualDesk;
        const std::string newTitle = hasTitle ? root["title"].asString() : std::string();
        auto out = std::make_shared<Json::Value>(Json::nullValue);
        const bool ran = cuOnDesktopThread([d, key, action, hasPos, hasSize, hasTitle,
                                            px, py, pw, ph, newTitle, out]() {
            JZVirtualWindow* w = cuVirtualFind(d, key);
            if (!w)
                return;
            if (action == "close") {
                // 同真系统点 X / HTTP 的 DELETE: 发 close 消息（应用可拒绝）, **不等它处理完**
                Json::Value o(Json::objectValue);
                o["handle"] = cuHandleStr(w);
                o["title"] = w->title();
                o["closing"] = true;
                w->close();
                *out = o;
                return;
            }
            if (action == "activate") {
                if (w->isMinimized())
                    w->setMinimized(false);         // 激活一个最小化的窗口顺手还原（窗口管理器同语义）
                d->activate(w);
            } else if (action == "minimize") {
                w->setMinimized(true);
            } else if (action == "maximize") {
                if (w->isMinimized())
                    w->setMinimized(false);
                w->setMaximized(true);
            } else if (action == "restore") {
                if (w->isMinimized())
                    w->setMinimized(false);
                if (w->isMaximized())
                    w->setMaximized(false);
            }
            // x,y = **外框**位置; w,h = **客户区**尺寸 —— 对外物理像素 ⇒ 进来先换逻辑
            if (hasSize)
                w->setClientSize(d->toLogical((int)lround(pw)), d->toLogical((int)lround(ph)));
            if (hasPos)
                w->setFramePos(d->toLogical((int)lround(px)), d->toLogical((int)lround(py)));
            if (hasTitle)
                w->setTitle(newTitle);
            d->refresh();
            const std::vector<JZVirtualWindow*> list = d->windowList();
            int idx = -1;
            for (size_t i = 0; i < list.size(); ++i)
                if (list[i] == w)
                    idx = (int)i;
            *out = cuVirtualWindowJson(d, w, idx);
        });
        return ran ? *out : Json::Value();
    }

    // 真桌面: SetWindowPos / ShowWindow / WM_CLOSE / SetWindowTextW
    HWND h = cuWin32Find(key);
    if (!h)
        return Json::Value();
    if (action == "activate")
        bringToFront(h);
    else if (action == "minimize")
        ShowWindow(h, SW_MINIMIZE);
    else if (action == "maximize")
        ShowWindow(h, SW_MAXIMIZE);
    else if (action == "restore")
        ShowWindow(h, SW_RESTORE);
    else if (action == "close")
        PostMessageW(h, WM_CLOSE, 0, 0);            // 与真系统点 X 同语义（投出去就返回）
    if (hasPos || hasSize) {
        RECT cur = {};
        GetWindowRect(h, &cur);
        int nx = hasPos ? (int)lround(px) : (int)cur.left;
        int ny = hasPos ? (int)lround(py) : (int)cur.top;
        int nw = (int)(cur.right - cur.left);
        int nh = (int)(cur.bottom - cur.top);
        if (hasSize) {
            // 客户区尺寸 -> 外框尺寸: 装饰差随该窗口的 style/exstyle 走（不抄常量）
            RECT r = { 0, 0, (LONG)lround(pw), (LONG)lround(ph) };
            AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrW(h, GWL_STYLE),
                               GetMenu(h) != nullptr,
                               (DWORD)GetWindowLongPtrW(h, GWL_EXSTYLE));
            nw = (int)(r.right - r.left);
            nh = (int)(r.bottom - r.top);
        }
        SetWindowPos(h, nullptr, nx, ny, nw, nh, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (hasTitle)
        SetWindowTextW(h, cuWide(root["title"].asString()).c_str());
    return cuWin32WindowJson(h, cuWin32IndexOf(h));
}
