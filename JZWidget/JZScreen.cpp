#include "JZScreen.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cmath>
#include <cstring>
#include <vector>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002   // Win8.1+: 含 DWM 合成内容
#endif

namespace
{

// MONITOR_DPI_TYPE (shcore.h): 动态加载, 故在此本地定义取值。
const UINT kMdtEffectiveDpi = 0;   // 含系统缩放的 DPI (等价 Qt logical DPI)
const UINT kMdtRawDpi       = 2;   // 面板物理 DPI (等价 Qt physical DPI)

typedef HRESULT(WINAPI* FnGetDpiForMonitor)(HMONITOR, UINT, UINT*, UINT*);
typedef UINT(WINAPI* FnGetDpiForWindow)(HWND);

// 延迟加载: Win8.1 之前没有 GetDpiForMonitor。
FnGetDpiForMonitor jzLoadGetDpiForMonitor()
{
    static bool loaded = false;
    static FnGetDpiForMonitor fn = nullptr;
    if (!loaded)
    {
        loaded = true;
        if (HMODULE shcore = LoadLibraryW(L"shcore.dll"))
            fn = reinterpret_cast<FnGetDpiForMonitor>(
                GetProcAddress(shcore, "GetDpiForMonitor"));
    }
    return fn;
}

// Win10 1607+ 才有 GetDpiForWindow。
FnGetDpiForWindow jzLoadGetDpiForWindow()
{
    static bool loaded = false;
    static FnGetDpiForWindow fn = nullptr;
    if (!loaded)
    {
        loaded = true;
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
            fn = reinterpret_cast<FnGetDpiForWindow>(
                GetProcAddress(user32, "GetDpiForWindow"));
    }
    return fn;
}

struct MonitorEntry
{
    HMONITOR hmon = nullptr;
    MONITORINFOEXW mi;
};

BOOL CALLBACK jzMonitorEnumProc(HMONITOR hmon, HDC, LPRECT, LPARAM lparam)
{
    auto* out = reinterpret_cast<std::vector<MonitorEntry>*>(lparam);
    MonitorEntry e;
    ZeroMemory(&e.mi, sizeof(e.mi));
    e.mi.cbSize = sizeof(MONITORINFOEXW);
    e.hmon = hmon;
    if (GetMonitorInfoW(hmon, reinterpret_cast<MONITORINFO*>(&e.mi)))
        out->push_back(e);
    return TRUE;
}

std::vector<MonitorEntry> jzEnumMonitors()
{
    std::vector<MonitorEntry> list;
    EnumDisplayMonitors(nullptr, nullptr, jzMonitorEnumProc,
                        reinterpret_cast<LPARAM>(&list));
    return list;
}

// DPI 查询链末端: GDI 兜底 (deviceName 为空 = 主屏)
UINT jzFallbackDpi(const wchar_t* deviceName)
{
    HDC dc = nullptr;
    bool created = false;
    if (deviceName && deviceName[0])
    {
        dc = CreateDCW(L"DISPLAY", deviceName, nullptr, nullptr);
        created = (dc != nullptr);
    }
    if (!dc)
        dc = GetDC(nullptr);

    const UINT dpi = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 0;
    if (dc)
    {
        if (created)
            DeleteDC(dc);
        else
            ReleaseDC(nullptr, dc);
    }
    return dpi > 0 ? dpi : 96;
}

UINT jzMonitorDpi(HMONITOR hmon, const wchar_t* deviceName, UINT which)
{
    UINT dx = 0, dy = 0;
    if (FnGetDpiForMonitor fn = jzLoadGetDpiForMonitor())
    {
        if (hmon && SUCCEEDED(fn(hmon, which, &dx, &dy)) && dx > 0)
            return dx;
    }
    return jzFallbackDpi(deviceName);
}

std::string jzWideToUtf8(const wchar_t* ws)
{
    if (!ws || !ws[0])
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    std::string out((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, &out[0], n, nullptr, nullptr);
    return out;
}

// DIB 抓图目标: 32bpp + **top-down** (biHeight 取负, 免去行序翻转)
struct DibTarget
{
    HDC mem = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ old = nullptr;
    void* bits = nullptr;
    int w = 0;
    int h = 0;

    bool ok() const { return mem && dib && bits; }
    void release()
    {
        if (mem && old)
            SelectObject(mem, old);
        if (dib)
            DeleteObject(dib);
        if (mem)
            DeleteDC(mem);
        mem = nullptr;
        dib = nullptr;
        old = nullptr;
        bits = nullptr;
    }

    // DIB 为 BGRA 且 alpha 未定义 -> 转 RGBA 且统一不透明
    JZImage toImage() const
    {
        if (!ok())
            return JZImage();
        const size_t n = (size_t)w * h;
        std::vector<uint8_t> rgba(n * 4);
        const uint8_t* src = static_cast<const uint8_t*>(bits);
        for (size_t i = 0; i < n; i++)
        {
            rgba[i * 4 + 0] = src[i * 4 + 2];   // R
            rgba[i * 4 + 1] = src[i * 4 + 1];   // G
            rgba[i * 4 + 2] = src[i * 4 + 0];   // B
            rgba[i * 4 + 3] = 255;              // A
        }
        return JZImage::fromRGBA(rgba.data(), w, h);
    }
};

DibTarget jzCreateDib(HDC ref, int w, int h)
{
    DibTarget t;
    if (!ref || w <= 0 || h <= 0)
        return t;

    t.mem = CreateCompatibleDC(ref);
    if (!t.mem)
        return t;

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;      // 负值 = top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    t.dib = CreateDIBSection(t.mem, &bi, DIB_RGB_COLORS, &t.bits, nullptr, 0);
    if (!t.dib || !t.bits)
    {
        t.bits = nullptr;
        t.release();
        return t;
    }
    t.old = SelectObject(t.mem, t.dib);
    t.w = w;
    t.h = h;
    return t;
}

} // namespace

// ==================== JZScreen ====================

struct JZScreen::Info
{
    HMONITOR hmon = nullptr;
    int index = -1;
    bool primary = false;
    RECT rc = { 0, 0, 0, 0 };        // rcMonitor (物理像素, 虚拟屏幕坐标系)
    RECT work = { 0, 0, 0, 0 };      // rcWork
    UINT logicalDpi = 96;
    UINT rawDpi = 96;
    std::wstring deviceName;
};

JZScreen::JZScreen()
{
}

JZScreen::JZScreen(int index)
{
    const std::vector<MonitorEntry> list = jzEnumMonitors();
    if (index < 0 || index >= (int)list.size())
        return;

    const MonitorEntry& e = list[index];
    m_info = std::make_shared<Info>();
    m_info->hmon = e.hmon;
    m_info->index = index;
    m_info->primary = (e.mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    m_info->rc = e.mi.rcMonitor;
    m_info->work = e.mi.rcWork;
    m_info->deviceName = e.mi.szDevice;
    m_info->logicalDpi = jzMonitorDpi(e.hmon, e.mi.szDevice, kMdtEffectiveDpi);
    m_info->rawDpi = jzMonitorDpi(e.hmon, e.mi.szDevice, kMdtRawDpi);
}

bool JZScreen::isValid() const
{
    return m_info != nullptr;
}

int JZScreen::index() const
{
    return m_info ? m_info->index : -1;
}

std::string JZScreen::name() const
{
    return m_info ? jzWideToUtf8(m_info->deviceName.c_str()) : std::string();
}

bool JZScreen::isPrimary() const
{
    return m_info && m_info->primary;
}

JZRect JZScreen::geometry() const
{
    if (!m_info)
        return JZRect();
    const float s = devicePixelRatio();
    return JZRect((int)std::lround(m_info->rc.left / s),
                  (int)std::lround(m_info->rc.top / s),
                  (int)std::lround((m_info->rc.right - m_info->rc.left) / s),
                  (int)std::lround((m_info->rc.bottom - m_info->rc.top) / s));
}

JZRect JZScreen::availableGeometry() const
{
    if (!m_info)
        return JZRect();
    const float s = devicePixelRatio();
    return JZRect((int)std::lround(m_info->work.left / s),
                  (int)std::lround(m_info->work.top / s),
                  (int)std::lround((m_info->work.right - m_info->work.left) / s),
                  (int)std::lround((m_info->work.bottom - m_info->work.top) / s));
}

float JZScreen::logicalDotsPerInch() const
{
    return m_info ? (float)m_info->logicalDpi : 96.0f;
}

float JZScreen::physicalDotsPerInch() const
{
    // 面板物理 DPI 不可得 (旧系统 / 驱动未上报) 时回落逻辑 DPI
    if (m_info && m_info->rawDpi > 0)
        return (float)m_info->rawDpi;
    return logicalDotsPerInch();
}

float JZScreen::devicePixelRatio() const
{
    return logicalDotsPerInch() / 96.0f;
}

JZImage JZScreen::grabScreen() const
{
    if (!m_info)
        return JZImage();

    const int x = m_info->rc.left;
    const int y = m_info->rc.top;
    const int w = m_info->rc.right - m_info->rc.left;
    const int h = m_info->rc.bottom - m_info->rc.top;
    if (w <= 0 || h <= 0)
        return JZImage();

    // GetDC(nullptr) = 虚拟屏幕 DC (覆盖所有显示器), 坐标即虚拟屏幕坐标系
    HDC sdc = GetDC(nullptr);
    if (!sdc)
        return JZImage();

    DibTarget t = jzCreateDib(sdc, w, h);
    JZImage img;
    // CAPTUREBLT: 否则抓不到分层/半透明窗口
    if (t.ok() && BitBlt(t.mem, 0, 0, w, h, sdc, x, y, SRCCOPY | CAPTUREBLT))
        img = t.toImage();

    t.release();
    ReleaseDC(nullptr, sdc);
    return img;
}

int JZScreen::count()
{
    return (int)jzEnumMonitors().size();
}

JZScreen JZScreen::primaryScreen()
{
    const std::vector<MonitorEntry> list = jzEnumMonitors();
    for (size_t i = 0; i < list.size(); i++)
    {
        if (list[i].mi.dwFlags & MONITORINFOF_PRIMARY)
            return JZScreen((int)i);
    }
    return list.empty() ? JZScreen() : JZScreen(0);
}

JZScreen JZScreen::screenAt(const JZPoint& globalPos)
{
    const std::vector<MonitorEntry> list = jzEnumMonitors();
    if (list.empty())
        return JZScreen();

    // 逻辑 -> 物理: 以主屏比例近似 (混合 DPI 多屏下, 仅在跨屏边界处有偏差)
    const float s = primaryScreen().devicePixelRatio();
    POINT pt = { (LONG)std::lround(globalPos.x() * s),
                 (LONG)std::lround(globalPos.y() * s) };

    const HMONITOR hmon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    for (size_t i = 0; i < list.size(); i++)
    {
        if (list[i].hmon == hmon)
            return JZScreen((int)i);
    }
    return JZScreen(0);
}

JZImage JZScreen::grabWindow(void* nativeHandle)
{
    HWND hwnd = reinterpret_cast<HWND>(nativeHandle);
    if (!hwnd)
        return JZImage();

    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0)
        return JZImage();

    HDC wdc = GetDC(hwnd);
    if (!wdc)
        return JZImage();

    JZImage img;
    // ⚠ PrintWindow 默认画的是**整个窗口**(含非客户区: 标题栏 + 左右下边框), 而目标 DIB 只有客户区大小
    //   ⇒ 直接抓会得到"带标题栏、底部被裁"的图: 客户区原点整体下移一个标题栏高度, 与 hitTest /
    //   语义树 / 合成点击 / 控件 geometry 用的**客户区坐标**全部对不上 (JZWidget::grab 的裁剪也一并错)。
    //   正确做法: 先按"整窗尺寸"抓完整窗口, 再按客户区原点在窗口里的偏移裁出客户区。
    RECT wr = { 0, 0, 0, 0 };
    POINT org = { 0, 0 };
    if (GetWindowRect(hwnd, &wr) && ClientToScreen(hwnd, &org))
    {
        const int ww = wr.right - wr.left;
        const int wh = wr.bottom - wr.top;
        const int ox = org.x - wr.left;          // 客户区原点相对窗口原点的偏移 (物理像素)
        const int oy = org.y - wr.top;
        if (ww >= w && wh >= h && ox >= 0 && oy >= 0 && ox + w <= ww && oy + h <= wh)
        {
            DibTarget full = jzCreateDib(wdc, ww, wh);
            if (full.ok() && PrintWindow(hwnd, full.mem, PW_RENDERFULLCONTENT))
            {
                DibTarget t = jzCreateDib(wdc, w, h);
                if (t.ok() && BitBlt(t.mem, 0, 0, w, h, full.mem, ox, oy, SRCCOPY))
                    img = t.toImage();
                t.release();
            }
            full.release();
        }
    }

    // 退化: 直接从窗口 DC 拷客户区 (GetDC(hwnd) 的 (0,0) 就是客户区原点; 会被遮挡影响, 但不会错位)
    if (img.isNull())
    {
        DibTarget t = jzCreateDib(wdc, w, h);
        if (t.ok() && BitBlt(t.mem, 0, 0, w, h, wdc, 0, 0, SRCCOPY | CAPTUREBLT))
            img = t.toImage();
        t.release();
    }

    ReleaseDC(hwnd, wdc);
    return img;
}

float JZScreen::logicalDpiFor(void* nativeHandle)
{
    const HWND hwnd = reinterpret_cast<HWND>(nativeHandle);
    const HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);

    // 1) GetDpiForMonitor (Win8.1+, per-monitor 准确)
    if (FnGetDpiForMonitor fn = jzLoadGetDpiForMonitor())
    {
        if (hmon)
        {
            UINT dx = 0, dy = 0;
            if (SUCCEEDED(fn(hmon, kMdtEffectiveDpi, &dx, &dy)) && dx > 0)
                return (float)dx;
        }
    }

    // 2) GetDpiForWindow (Win10 1607+)
    if (FnGetDpiForWindow fn = jzLoadGetDpiForWindow())
    {
        if (hwnd)
        {
            const UINT d = fn(hwnd);
            if (d > 0)
                return (float)d;
        }
    }

    // 3) 主屏 GDI 兜底
    return (float)jzFallbackDpi(nullptr);
}
