#include "JZComputerUse.h"
#include <windows.h>
#include <map>
#include <vector>
#include <sstream>
#include <algorithm>
#include <opencv2/opencv.hpp>
#include <shlobj.h>      // IShellDispatch4 / CLSID_Shell (显示桌面)
#include <objbase.h>     // CoInitializeEx / CoCreateInstance / IID_PPV_ARGS
#include <tlhelp32.h>    // 进程快照
#include <cstring>       // strstr

CuRect JZComputerUse::s_lastLogical;

namespace {
    std::string tempDir() {
        char buf[MAX_PATH];
        if (GetTempPathA(MAX_PATH, buf) > 0) return std::string(buf);
        return std::string(".\\");
    }

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
    DWORD pidByName(const std::string& name) {
        DWORD pid = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32A pe = {}; pe.dwSize = sizeof(pe);
        for (BOOL b = Process32FirstA(snap, &pe); b; b = Process32NextA(snap, &pe)) {
            std::string p = pe.szExeFile;
            if (p == name || (name.back() != '.' && p == name + ".exe")) { pid = pe.th32ProcessID; break; }
        }
        CloseHandle(snap);
        return pid;
    }
}

void JZComputerUse::updateLogical(const JZRect& r) {
    s_lastLogical = { (double)r.x(), (double)r.y(), (double)r.width(), (double)r.height() };
}

// 归一化(相对虚拟桌面逻辑矩形) -> 虚拟屏幕绝对坐标(0..65535, SendInput 用)
void JZComputerUse::toAbsolute(double nx, double ny, DWORD& ax, DWORD& ay) {
    int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsW = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    int vsH = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    double px = s_lastLogical.x + nx * s_lastLogical.w;
    double py = s_lastLogical.y + ny * s_lastLogical.h;
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
    DWORD ax, ay; toAbsolute(nx, ny, ax, ay);
    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE; in[0].mi.dx = ax; in[0].mi.dy = ay; in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    in[1].type = INPUT_MOUSE; in[1].mi.dx = ax; in[1].mi.dy = ay; in[1].mi.dwFlags = btnDown(btn) | MOUSEEVENTF_ABSOLUTE;
    SendInput(2, in, sizeof(INPUT));
    Sleep(20);
    in[1].mi.dwFlags = btnUp(btn) | MOUSEEVENTF_ABSOLUTE;
    SendInput(2, in, sizeof(INPUT));
    return true;
}

bool JZComputerUse::doubleClick(double nx, double ny, MouseBtn btn) {
    click(nx, ny, btn); Sleep(20); click(nx, ny, btn); return true;
}

bool JZComputerUse::drag(double x1, double y1, double x2, double y2) {
    DWORD ax1, ay1, ax2, ay2; toAbsolute(x1, y1, ax1, ay1); toAbsolute(x2, y2, ax2, ay2);
    INPUT in[4] = {};
    in[0].type = INPUT_MOUSE; in[0].mi.dx = ax1; in[0].mi.dy = ay1; in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    in[1].type = INPUT_MOUSE; in[1].mi.dx = ax1; in[1].mi.dy = ay1; in[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_ABSOLUTE;
    in[2].type = INPUT_MOUSE; in[2].mi.dx = ax2; in[2].mi.dy = ay2; in[2].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    in[3].type = INPUT_MOUSE; in[3].mi.dx = ax2; in[3].mi.dy = ay2; in[3].mi.dwFlags = MOUSEEVENTF_LEFTUP | MOUSEEVENTF_ABSOLUTE;
    SendInput(4, in, sizeof(INPUT));
    return true;
}

void JZComputerUse::key(const std::string& name) {
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
    JZRect logical;
    JZImage img = JZScreenShot::captureVirtualDesktop(&logical);
    updateLogical(logical);
    return JZScreenShot::toDataUrl(img);
}

CuRect JZComputerUse::matchImage(const std::string& templatePath, const CuRect& region) {
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
}

CuRect JZComputerUse::matchColor(const std::string& hexColor, const CuRect& region) {
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
}

// ————————————————————————————————————————————————
// 进程与窗口管理（不使用按键模拟）
// ————————————————————————————————————————————————
DWORD JZComputerUse::startProcess(const std::string& path, const std::string& args) {
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
    FindCtx c; c.title = &title;
    EnumWindows(EnumFindTitle, (LPARAM)&c);
    if (!c.found) return false;
    bringToFront(c.found);
    return true;
}
