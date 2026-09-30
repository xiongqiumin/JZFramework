#include "JZGdiBackend.h"

#include "JZLayer.h"
#include "JZDrawTask.h"
#include "JZImage.h"
#include "JZPixmap.h"
#include "JZFontMetrics.h"
#include "JZDisplayDevice.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>   // IStream/PROPID: WIN32_LEAN_AND_MEAN 会排除 ole2, GDI+ 需要

// GDI+ 的头文件里使用了无限定的 min/max。上面定义了 NOMINMAX 会屏蔽掉
// windows.h 的 min/max 宏, 导致 GdiplusTypes.h 报 C3861; 这里在 Gdiplus
// 命名空间内引入 std 版本供其使用 (VS2015 + Win8.1 SDK 下必现)。
#include <algorithm>
namespace Gdiplus {
    using std::min;
    using std::max;
}

#include <gdiplus.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace Gdiplus;

namespace {

// 全框架文本测量/绘制的**唯一** StringFormat, 语义对齐 Qt QFontMetrics:
// 空格 (含尾随空格/纯空格) 一律按字形计宽并绘制。
//   - GenericTypographic: 紧贴字形, 无 GenericDefault 的 ~1/6em 内边距 (居中/光标定位必须);
//   - MeasureTrailingSpaces: GDI+ 默认会忽略字符串末尾的空格 (测 0 宽/不绘制),
//     与 Qt 语义相悖, 必须显式补上。
// 测量 (GdiFontEngine::textWidth) 与绘制 (Text 任务) 共用这一份, 不允许出现第二种
// "忽略尾随空格"的格式, 否则光标/点击定位与实际绘制会错开。
// 注: StringFormat 拷贝构造是 protected, 只能从 GenericTypographic() 的共享单例指针
// clone 出新实例再改 flag (绝不改单例本身), 故这里返回 unique_ptr 由调用方持有。
std::unique_ptr<StringFormat> jzTextFormat()
{
    auto fmt = std::make_unique<StringFormat>(StringFormat::GenericTypographic());
    fmt->SetFormatFlags(fmt->GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces);
    return fmt;
}

} // namespace

// ==================== GDI+ 进程级生命周期 (多窗口共享, 引用计数) ====================
namespace {

// 字体族缓存的清理 (定义见下面的"字体族缓存"小节)。
// 必须在 GdiplusShutdown 之前调用: GDI+ 一旦关闭, 再析构任何 GDI+ 对象都会崩。
void jzReleaseFontFamily();

struct GdiplusLifetime
{
    ULONG_PTR token = 0;
    int refCount = 0;

    bool acquire()
    {
        if (refCount++ == 0) {
            GdiplusStartupInput input;
            if (GdiplusStartup(&token, &input, nullptr) != Ok) {
                refCount = 0;
                return false;
            }
        }
        return true;
    }

    // GDI+ 是否还活着。绘制前用它兜底：一旦已 Shutdown，再绘制会进 GdiPlus.dll 崩。
    bool isReady() const { return token != 0; }

    void release()
    {
        if (refCount > 0 && --refCount == 0) {
            jzReleaseFontFamily();      // 缓存的 GDI+ 对象必须先于 Shutdown 销毁
            GdiplusShutdown(token);
            token = 0;
        }
    }
};

GdiplusLifetime& gdiplusLifetime()
{
    static GdiplusLifetime life;
    return life;
}

} // namespace

// ==================== 绘制辅助 ====================
static Color toGdiColor(const JZColor& c, uint8_t opacity)
{
    uint8_t a = (uint8_t)((c.alpha() * opacity) / 255);
    return Color(a, c.red(), c.green(), c.blue());
}

// JZRect -> GDI+ RectF (世界坐标, 1:1 映射)。几何以本地坐标提交,
// 缩放/DPI/偏移由 Graphics 的世界变换 (SetTransform) 统一施加, 这里不再乘 scale。
static RectF toGdiRect(const JZRect& rc)
{
    return RectF((REAL)rc.x(), (REAL)rc.y(), (REAL)rc.width(), (REAL)rc.height());
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

// 贴一张 JZPixmap: 零拷贝把它持有的像素缓冲包成 GDI+ 位图后 DrawImage。
//   - JZPixmap 的像素已经是 32bppPARGB (后端原生布局), 所以这里只做"包一层";
//   - GDI+ 的 Bitmap(w,h,stride,fmt,scan0) 构造函数**只记指针不复制像素**, 而
//     缓冲由 JZPixmap/shared_ptr 持有, 会活过整个绘制, 不存在悬垂; 位图本身是
//     本作用的局部对象, 绝不跨帧缓存 (旧实现缓存了"指向临时缓冲的位图"才崩的);
//   - 每帧的代价就是一次普通位图拷贝 (4K 实测 ~10ms)。
static void jzDrawPixmap(Graphics& g, const std::shared_ptr<JZPixmapData>& pm, const RectF& dest)
{
    if (!pm || !pm->pixels() || pm->width() <= 0 || pm->height() <= 0)
        return;

    const int w = pm->width();
    const int h = pm->height();
    Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)pm->pixels());
    if (bmp.GetLastStatus() != Ok)
        return;

    // 目标尺寸与源尺寸只差取整误差 (逻辑坐标 * scale 的小数) 时, 吸附成整数矩形:
    // GDI+ 只有源/目标完全对齐才走直拷路径, 差零点几像素就会整图重采样 —— 表现为
    // "整屏截图发糊"。这里把取整误差吃掉, 真正的缩放 (差异更大) 仍走插值。
    if (std::fabs(dest.Width - w) <= 0.5f && std::fabs(dest.Height - h) <= 0.5f)
        g.DrawImage(&bmp, Rect((INT)std::lround(dest.X), (INT)std::lround(dest.Y), w, h));
    else
        g.DrawImage(&bmp, dest);
}

// 圆角矩形路径 (radius 已含缩放)
static void addRoundRectPath(GraphicsPath& path, const RectF& r, float radius)
{
    if (radius <= 0.0f) {
        path.AddRectangle(r);
        return;
    }
    float d = radius * 2.0f;
    if (d > r.Width)  d = r.Width;
    if (d > r.Height) d = r.Height;
    radius = d * 0.5f;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

// ==================== 字体族缓存 ====================
// 刻意**不用**函数内 static FontFamily: 那会在 main() 返回后的静态析构阶段才销毁,
// 而 GDI+ 随最后一个窗口的销毁就已经 GdiplusShutdown() 了 —— 在已关闭的 GDI+ 上
// 析构对象, 会在 FontFamily::~FontFamily 里进 GdiPlus.dll 访问已释放状态直接崩。
// (位图缓存、字体度量引擎同理, 都在 JZGdiBackend::destroy() 里提前释放。)
namespace {

FontFamily* g_fontFamily = nullptr;      // 主字体: 微软雅黑
FontFamily* g_fallbackFamily = nullptr;  // 回退字体: 由本文件管理生命周期
std::map<std::string, FontFamily*> g_namedFamilies;  // 用户指定字体族 (按名字缓存, 值为 nullptr = 该名不可用)

// 文本字体族 (优先微软雅黑)
const FontFamily& jzFontFamily()
{
    if (!g_fontFamily)
        g_fontFamily = new FontFamily(L"Microsoft YaHei");
    return *g_fontFamily;
}

// 回退字体族。
// 不能用 FontFamily::GenericSansSerif(): 它是 GDI+ 内部的**函数内 static**,
// 要等到 main() 返回后的静态析构阶段才销毁, 而 GDI+ 随最后一个窗口的销毁就
// GdiplusShutdown() 了 —— 在已关闭的 GDI+ 上析构对象, 会在 GdiPlus.dll 里直接崩。
// (这正是上面注释里要避开的坑, 之前 probeFamily/replayTasks 里却仍在用它。)
// 这里改成自己 new、由 jzReleaseFontFamily() 在 Shutdown 之前释放。
const FontFamily& jzFallbackFamily()
{
    if (!g_fallbackFamily)
    {
        g_fallbackFamily = new FontFamily(L"Microsoft Sans Serif");
        if (g_fallbackFamily->GetLastStatus() != Ok)
        {
            delete g_fallbackFamily;
            g_fallbackFamily = new FontFamily(L"Arial");
        }
    }
    return *g_fallbackFamily;
}

// 按名字解析字体族: 空名 = 默认 (微软雅黑); 命名族先查缓存, 创建失败 (系统中
// 无此字体) 则记一笔 nullptr 并回退到回退族。所有缓存对象由 jzReleaseFontFamily
// 在 GdiplusShutdown 之前统一释放 (绝不用函数内 static, 否则会在已关闭的 GDI+ 上崩)。
const FontFamily* jzResolveFamily(const std::string& name)
{
    if (name.empty())
        return &jzFontFamily();
    auto it = g_namedFamilies.find(name);
    if (it != g_namedFamilies.end())
        return it->second ? it->second : &jzFallbackFamily();
    FontFamily* fam = new FontFamily(utf8ToUtf16(name).c_str());
    if (fam->GetLastStatus() != Ok) {
        delete fam;
        g_namedFamilies[name] = nullptr;   // 记住此名不可用, 之后直接走回退
        return &jzFallbackFamily();
    }
    g_namedFamilies[name] = fam;
    return fam;
}

// JZFont::Style 位或 -> GDI+ FontStyle (粗体/斜体/下划线/删除线)
int jzGdiFontStyle(int jzStyle)
{
    int st = FontStyleRegular;
    if (jzStyle & JZFont::Bold)      st |= FontStyleBold;
    if (jzStyle & JZFont::Italic)    st |= FontStyleItalic;
    if (jzStyle & JZFont::Underline) st |= FontStyleUnderline;
    if (jzStyle & JZFont::StrikeOut) st |= FontStyleStrikeout;
    return st;
}

void jzReleaseFontFamily()
{
    delete g_fontFamily;
    g_fontFamily = nullptr;
    delete g_fallbackFamily;
    g_fallbackFamily = nullptr;
    for (auto& kv : g_namedFamilies)
        delete kv.second;
    g_namedFamilies.clear();
}

} // namespace

// ==================== 字体度量引擎 (注册到 JZFontMetrics) ====================
namespace {

class GdiFontEngine : public JZFontEngine
{
public:
    GdiFontEngine()
    {
        m_hdc = CreateCompatibleDC(nullptr);
        m_graphics = new Graphics(m_hdc);
        m_graphics->SetTextRenderingHint(TextRenderingHintAntiAlias);
    }

    ~GdiFontEngine() override
    {
        delete m_graphics;
        m_graphics = nullptr;
        if (m_hdc) {
            DeleteDC(m_hdc);
            m_hdc = nullptr;
        }
    }

    int textWidth(const std::string& text, const JZFont& font) override
    {
        std::wstring wtext = utf8ToUtf16(text);
        if (wtext.empty())
            return 0;

        const FontFamily* fam = jzResolveFamily(font.family());
        Font primary(fam, (REAL)font.size(), (FontStyle)jzGdiFontStyle(font.style()), UnitPixel);
        Font fallback(&jzFontFamily(), (REAL)font.size(), FontStyleRegular, UnitPixel);
        const Font* use = (primary.GetLastStatus() == Ok) ? &primary : &fallback;
        RectF layout(0.0f, 0.0f, 1000000.0f, (REAL)(font.size() * 4));
        RectF bounds;
        // 统一 Qt 语义的文本格式 (见 jzTextFormat): 空格照常计宽,
        // 与绘制侧 (JZTextTask) 严格一致, 保证光标/滚动/点击定位不偏。
        std::unique_ptr<StringFormat> fmt = jzTextFormat();
        m_graphics->MeasureString(wtext.c_str(), (INT)wtext.size(), use, layout, fmt.get(), &bounds);
        return (int)(bounds.Width + 0.5f);
    }

    int textHeight(const JZFont& font) override
    {
        const FontFamily* fam = jzResolveFamily(font.family());
        Font primary(fam, (REAL)font.size(), FontStyleRegular, UnitPixel);
        Font fallback(&jzFontFamily(), (REAL)font.size(), FontStyleRegular, UnitPixel);
        const Font* use = (primary.GetLastStatus() == Ok) ? &primary : &fallback;
        return (int)(use->GetHeight(m_graphics) + 0.5f);
    }

    // 以下三个供文本控件做光标定位与垂直居中:
    // GDI+ 的 CellAscent/CellDescent/LineSpacing 是"设计单位"(以 em 为单位),
    // 需按 emHeight 换算到像素; LineSpacing - Ascent - Descent 即 internalLeading
    // (位于字符 cell 顶部、字形之上的空白), 不扣掉它文字会视觉偏低。
    // 字形 cell 度量只取决于字体族 + 字号 (与风格无关), 故用 FontStyleRegular。
    int ascent(const JZFont& font) override
    {
        const FontFamily* fam = jzResolveFamily(font.family());
        return designToPixel(fam->GetCellAscent(FontStyleRegular), fam, font.size());
    }

    int descent(const JZFont& font) override
    {
        const FontFamily* fam = jzResolveFamily(font.family());
        return designToPixel(fam->GetCellDescent(FontStyleRegular), fam, font.size());
    }

    int internalLeading(const JZFont& font) override
    {
        const FontFamily* fam = jzResolveFamily(font.family());
        int spacing = designToPixel(fam->GetLineSpacing(FontStyleRegular), fam, font.size());
        int rest = spacing - ascent(font) - descent(font);
        return (rest > 0) ? rest : 0;
    }

private:
    // 设计单位 -> 像素 (em 高度来自指定字体族)
    int designToPixel(UINT16 design, const FontFamily* fam, int pixelSize)
    {
        UINT16 em = fam->GetEmHeight(FontStyleRegular);
        if (em == 0)
            return 0;
        return (int)((float)design * pixelSize / (float)em + 0.5f);
    }

    HDC m_hdc = nullptr;
    Graphics* m_graphics = nullptr;
};

GdiFontEngine* s_fontEngine = nullptr;

int& fontEngineRefCount()
{
    static int refCount = 0;
    return refCount;
}

} // namespace

// ==================== 生命周期 ====================
JZGdiBackend::JZGdiBackend()
{
}

JZGdiBackend::~JZGdiBackend()
{
    destroy();
}

bool JZGdiBackend::acquireFontEngine()
{
    // GDI+ 先起来: GdiFontEngine 的测量全程依赖它 (Font/Graphics/MeasureString)。
    if (!gdiplusLifetime().acquire())
        return false;
    // 字体度量引擎 (进程级, 引用计数与 GDI+ 生命周期一致)
    if (fontEngineRefCount()++ == 0) {
        s_fontEngine = new GdiFontEngine();
        JZFontMetrics::setEngine(s_fontEngine);
    }
    return true;
}

void JZGdiBackend::releaseFontEngine()
{
    // 必须先于 GdiplusShutdown 注销: 引擎持有 GDI+ 对象 (Graphics/HDC)。
    if (fontEngineRefCount() > 0 && --fontEngineRefCount() == 0) {
        JZFontMetrics::setEngine(nullptr);
        delete s_fontEngine;
        s_fontEngine = nullptr;
    }
    gdiplusLifetime().release();   // 内部会先释放字体族缓存再 Shutdown
}

bool JZGdiBackend::init(JZDisplayDevice* device, void* hwnd, unsigned physicalW, unsigned physicalH)
{
    m_widgetDevice = device;
    // hwnd == nullptr = 离屏模式: 不需要窗口, 只要字体度量引擎 (GDI+) 可用。
    if (!acquireFontEngine())
        return false;
    m_initialized = true;      // 之后 destroy() 才允许减引用计数

    m_hwnd = hwnd;
    m_physW = physicalW;
    m_physH = physicalH;
    createMemBitmap(physicalW, physicalH);   // 尺寸为 0 时失败, render 会跳过
    return true;
}

void JZGdiBackend::destroy()
{
    // 缓存的 pixmap 是 CPU 侧像素缓冲 (不含 GDI+ 对象), 直接放掉即可;
    // 这里显式清空只是为了尽快释放那几十 MB。
    m_images.clear();

    releaseMemBitmap();
    m_hwnd = nullptr;

    // 引用计数的递减必须与 init() 里的递增严格对称。
    // 之前这里是无条件的，于是"构造了 backend 但 init 失败/从未 init"的对象在析构时
    // 也会减一次，把别的窗口仍在使用的 GDI+ 提前 Shutdown —— 表现就是关掉一个对话框后
    // 主窗口下一次重绘在 GdiPlus.dll 里崩。
    if (m_initialized) {
        m_initialized = false;
        releaseFontEngine();   // 字体度量引擎 + GDI+ 引用计数 (与 init 严格对称)
    }
}

// ==================== 双缓冲位图 ====================
bool JZGdiBackend::createMemBitmap(unsigned physicalW, unsigned physicalH)
{
    releaseMemBitmap();
    if (physicalW == 0 || physicalH == 0)
        return false;

    // 有窗口: 与窗口 DC 兼容; 离屏 (无窗口): 与屏幕 DC 兼容。
    // CreateCompatibleDC(nullptr) 是 Win32 允许的"屏幕兼容"形式, 不需要任何窗口
    // (现有先例: GdiFontEngine 构造函数)。
    HDC wdc = m_hwnd ? GetDC((HWND)m_hwnd) : nullptr;
    HDC dc = wdc ? wdc : CreateCompatibleDC(nullptr);
    if (!dc)
        return false;
    m_memDC = CreateCompatibleDC(dc);

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = (LONG)physicalW;
    bmi.bmiHeader.biHeight = -(LONG)physicalH;   // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    m_memBmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (wdc)
        ReleaseDC((HWND)m_hwnd, wdc);
    else
        DeleteDC(dc);

    if (!m_memDC || !m_memBmp) {
        releaseMemBitmap();
        return false;
    }
    // CreateDIBSection 的新内存未初始化。整窗渲染虽会全量重画, 但逻辑 rect*scale
    // 与客户区物理尺寸存在取整差时最外圈像素可能无人绘制 —— 预填白色兜底,
    // 杜绝边缘露出未初始化内存 (黑/杂色/透明花边)。
    if (bits)
        memset(bits, 0xFF, (size_t)physicalW * physicalH * 4);
    m_bits = bits;    // 帧缓冲像素首地址 (snapshot 取帧用)
    m_oldBmp = (void*)SelectObject((HDC)m_memDC, (HBITMAP)m_memBmp);
    return true;
}

void JZGdiBackend::releaseMemBitmap()
{
    if (m_memDC && m_memBmp)
        SelectObject((HDC)m_memDC, (HBITMAP)m_oldBmp);
    if (m_memDC) { DeleteDC((HDC)m_memDC);       m_memDC = nullptr; }
    if (m_memBmp) { DeleteObject((HBITMAP)m_memBmp); m_memBmp = nullptr; }
    m_bits = nullptr;
    m_oldBmp = nullptr;
}

void JZGdiBackend::resizeSwapChain(unsigned physicalW, unsigned physicalH)
{
    if (physicalW == 0 || physicalH == 0)
        return;
    // 尺寸没变 (拖动过程中的冗余 WM_SIZE 等) 就不重建位图:
    // 避免每帧重复分配 DIB + 清空内容 (拖动 resize 时同步渲染本就要尽量轻)。
    if (physicalW == m_physW && physicalH == m_physH)
        return;
    m_physW = physicalW;
    m_physH = physicalH;
    createMemBitmap(physicalW, physicalH);
}

void JZGdiBackend::rebuildFonts()
{
    // GDI+ 字体按绘制任务即时创建, DPI 变化无需重建
}

// ==================== 渲染 ====================
void JZGdiBackend::render(JZLayer* layer)
{
    if (!m_memDC || !layer)
        return;
    if (m_physW == 0 || m_physH == 0)
        return;
    // 兜底: GDI+ 已 Shutdown 时不再绘制，否则一进 GdiPlus.dll 就崩
    if (!gdiplusLifetime().isReady())
        return;

    // 淘汰源图已销毁的位图缓存 (放在回放前: 本帧要画的图由 draw task 持着
    // shared_ptr, 不会被误回收)
    purgeImageCache();

    {
        // 任务回放到内存位图 (保留上帧内容, 脏区由各任务的 clip 覆盖)
        Graphics g((HDC)m_memDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.SetCompositingQuality(CompositingQualityHighSpeed);
        replayTasks(&g, layer);
    }   // Graphics 先析构, 再提交到屏幕

    // 提交上屏: 仅绑定窗口时执行。离屏后端到此为止 —— 帧就留在 DIB 里, 由 snapshot 取走。
    if (m_hwnd) {
        HDC wdc = GetDC((HWND)m_hwnd);
        if (wdc) {
            BitBlt(wdc, 0, 0, (int)m_physW, (int)m_physH, (HDC)m_memDC, 0, 0, SRCCOPY);
            ReleaseDC((HWND)m_hwnd, wdc);
        }
    }

    layer->clearTasks();
}

// 取帧: 把帧缓冲 DIB 的像素 (top-down BGRA) 拷成 JZImage (RGBA8888)。
// 与"渲染出来的那一帧"逐像素一致 (即窗口上看到的内容), 不做抓屏/合成,
// 因此离屏设备同样能取帧, 且不受遮挡、z 序、前台、多屏影响。
bool JZGdiBackend::snapshot(JZImage& out)
{
    if (!m_bits || m_physW == 0 || m_physH == 0)
        return false;

    const size_t count = (size_t)m_physW * m_physH;
    std::vector<unsigned char> rgba(count * 4);
    const unsigned char* src = (const unsigned char*)m_bits;
    for (size_t i = 0; i < count; ++i) {
        rgba[i * 4 + 0] = src[i * 4 + 2];   // R <- B
        rgba[i * 4 + 1] = src[i * 4 + 1];   // G
        rgba[i * 4 + 2] = src[i * 4 + 0];   // B <- R
        rgba[i * 4 + 3] = src[i * 4 + 3];   // A
    }
    out = JZImage::fromRGBA(rgba.data(), (int)m_physW, (int)m_physH);
    return !out.isNull();
}

// 把绘制任务回放到任意 Graphics (窗口 DIB / 离屏位图共用同一份回放逻辑)。
//   s        逻辑 -> 物理像素的缩放 (窗口=设备 DPI; 离屏图=1, 逻辑单位即像素)
//   getPixmap 把 JZImageTask 的源图转成后端原生位图 (窗口走帧缓存懒转换; 离屏走本地缓存)
static void jzReplayLayer(Graphics& g, JZLayer* layer, float s,
                          std::function<std::shared_ptr<JZPixmapData>(const std::shared_ptr<JZImageData>&)> getPixmap)
{
    // 绘制流状态机: 几何任务只携带本地几何 + 不透明度, 样式 (pen/brush/font) 与裁剪/本地矩阵
    // 由 Push (保存并应用新状态) / Pop (回退) / Change (非栈式改当前) 维护。
    // 每个控件绘制前为单位矩阵 (本地坐标), 放置偏移 (ox,oy) 与 DPI 缩放由框架/后端补充, 而非
    // 控件作者的矩阵 —— 即不存在跨控件的"世界坐标", 矩阵只在控件内部生效。
    // 后端世界变换 = Scale(s) * Translate(ox,oy) * matrix, 几何以本地坐标提交, 由 GDI+ SetTransform
    // 统一施加; 因此笔宽/文本/图片/圆角都随变换正确缩放与旋转, 无需 painter 端烘焙矩阵。
    struct StreamState {
        JZPen pen;
        JZBrush brush;
        JZFont font;
        JZRect clip;
        JZMatrix2D transform;   // 控件本地变换 (单位阵为初值, 不含放置偏移)
        int ox = 0;             // 放置偏移: 控件在图层坐标中的位置 (框架设定, 控件作者无感)
        int oy = 0;
    };
    std::vector<StreamState> stack;
    StreamState cur;   // 默认: 空 clip (无裁剪) + 默认 pen/brush/font + 单位阵 + 偏移 0

    // 把当前状态应用到 Graphics: 设置世界变换与裁剪。
    // 裁剪以显示坐标表示, 经 (Translate(ox,oy)*matrix)^{-1} 折算回世界坐标后交给 GDI+, 由其用当前
    // 世界变换映射到设备坐标 (等价于旧 toGdiRect(clip, s); 单位阵时 worldClip=本地 rect, 完全一致)。
    auto applyState = [&]() {
        JZMatrix2D w = JZMatrix2D::scaling(s, s)
                           .multiplied(JZMatrix2D::translation(cur.ox, cur.oy))
                           .multiplied(cur.transform);
        Gdiplus::Matrix gm((REAL)w.at(0, 0), (REAL)w.at(1, 0), (REAL)w.at(0, 1),
                           (REAL)w.at(1, 1), (REAL)w.at(0, 2), (REAL)w.at(1, 2));
        g.SetTransform(&gm);

        if (cur.clip.isEmpty()) {
            g.ResetClip();
        } else {
            JZMatrix2D inv = JZMatrix2D::translation(cur.ox, cur.oy)
                                 .multiplied(cur.transform).inverse();
            double x0 = cur.clip.x(), y0 = cur.clip.y();
            double x1 = x0 + cur.clip.width(), y1 = y0 + cur.clip.height();
            double pts[4][2] = { {x0, y0}, {x1, y0}, {x1, y1}, {x0, y1} };
            double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300;
            for (int i = 0; i < 4; ++i) {
                double wx, wy; inv.map(pts[i][0], pts[i][1], wx, wy);
                minx = std::min(minx, wx); maxx = std::max(maxx, wx);
                miny = std::min(miny, wy); maxy = std::max(maxy, wy);
            }
            // floor/ceil 外扩到覆盖 AA 像素外缘, 避免边框被裁 (对齐旧逻辑)。
            INT rx = (INT)std::floor(minx), ry = (INT)std::floor(miny);
            INT rw = (INT)std::ceil(maxx) - rx, rh = (INT)std::ceil(maxy) - ry;
            g.SetClip(Rect(rx, ry, rw, rh));
        }
    };

    for (auto& task : layer->tasks())
    {
        switch (task->type) {
        case JZDrawTask::Push: {
            auto* t = static_cast<JZDrawPushTask*>(task.get());
            stack.push_back(cur);
            cur.pen = t->pen;
            cur.brush = t->brush;
            cur.font = t->font;
            cur.clip = t->clip;
            cur.transform = t->matrix;
            cur.ox = t->ox;
            cur.oy = t->oy;
            applyState();
            break;
        }
        case JZDrawTask::Pop: {
            if (!stack.empty()) {
                cur = stack.back();
                stack.pop_back();
            } else {
                cur = StreamState();
            }
            applyState();
            break;
        }
        case JZDrawTask::Change: {
            auto* t = static_cast<JZDrawChangeTask*>(task.get());
            if (t->pen.isSome())    cur.pen = t->pen.value();
            if (t->brush.isSome())  cur.brush = t->brush.value();
            if (t->font.isSome())   cur.font = t->font.value();
            if (t->clip.isSome())   cur.clip = t->clip.value();
            if (t->matrix.isSome()) cur.transform = t->matrix.value();
            applyState();
            break;
        }
        case JZDrawTask::Line: {
            auto* t = static_cast<JZLineTask*>(task.get());
            // 笔宽为世界坐标单位; 世界变换 (含 DPI scale 与用户矩阵) 自动缩放之。
            float w = (cur.pen.width() > 0 ? cur.pen.width() : 1);
            Pen pen(toGdiColor(cur.pen.color(), t->opacity), w);
            g.DrawLine(&pen, (REAL)t->x1, (REAL)t->y1, (REAL)t->x2, (REAL)t->y2);
            break;
        }
        case JZDrawTask::Rect: {
            auto* t = static_cast<JZFillRectTask*>(task.get());
            RectF r = toGdiRect(t->area);
            SolidBrush br(toGdiColor(t->color, t->opacity));
            if (t->radius > 0) {
                GraphicsPath p;
                addRoundRectPath(p, r, (REAL)t->radius);
                g.FillPath(&br, &p);
            } else {
                g.FillRectangle(&br, r);
            }
            break;
        }
        case JZDrawTask::Border: {
            auto* t = static_cast<JZBorderTask*>(task.get());
            float pw = (cur.pen.width() > 0 ? cur.pen.width() : 1);
            if (pw <= 0.0f)
                break;
            RectF r = toGdiRect(t->area);
            Pen pen(toGdiColor(cur.pen.color(), t->opacity), pw);

            // 描边几何须向内缩半笔宽, 与 Qt drawRect 语义对齐 (见旧注释)。此内缩在世界坐标进行,
            // 经世界变换后视觉内缩与旧实现一致 (整数 scale 下完全等价; 非整数/缩放时随变换正确缩放)。
            float half = pw * 0.5f;
            half = std::min(half, std::min(r.Width, r.Height) * 0.5f); // 过窄时防几何翻负
            RectF ir(r.X + half, r.Y + half,
                     r.Width - 2.0f * half, r.Height - 2.0f * half);
            if (t->radius > 0) {
                // 半径同步减 half: 内缩后圆弧圆心与 fillRect 圆角填充完全重合, 描边紧贴外缘。
                GraphicsPath p;
                addRoundRectPath(p, ir, std::max(0.0f, (REAL)t->radius - half));
                g.DrawPath(&pen, &p);
            } else {
                g.DrawRectangle(&pen, ir);
            }
            break;
        }
        case JZDrawTask::Arc: {
            auto* t = static_cast<JZArcTask*>(task.get());
            // 角度: 1/10 度, 0 = 3 点方向, 顺时针; GDI+ 屏幕坐标系 (y 向下) 正角度同为顺时针。
            float radius = (t->arcRadius > 0 ? (REAL)t->arcRadius : 1.0f);
            float sweep = (t->arcEndAngle - t->arcStartAngle) / 10.0f;
            if (sweep < 0.0f)
                sweep += 360.0f;
            if (sweep > 0.0f && radius > 0.0f) {
                float w = (cur.pen.width() > 0 ? cur.pen.width() : 1);
                Pen pen(toGdiColor(cur.pen.color(), t->opacity), w);
                RectF rc((REAL)t->arcCx - radius, (REAL)t->arcCy - radius,
                         radius * 2.0f, radius * 2.0f);
                g.DrawArc(&pen, rc, t->arcStartAngle / 10.0f, sweep);
            }
            break;
        }
        case JZDrawTask::Text: {
            auto* t = static_cast<JZTextTask*>(task.get());
            if (!t->text.empty()) {
                std::wstring wtext = utf8ToUtf16(t->text);
                // Font 禁止赋值, 先按字体描述解析可用字体族与风格, 再一次性构造
                const FontFamily* family = jzResolveFamily(cur.font.family());
                int style = jzGdiFontStyle(cur.font.style());
                Font primary(family, (REAL)cur.font.size(), (FontStyle)style, UnitPixel);
                Font fallback(&jzFontFamily(), (REAL)cur.font.size(), FontStyleRegular, UnitPixel);
                const Font* font = (primary.GetLastStatus() == Ok) ? &primary : &fallback;
                SolidBrush br(toGdiColor(cur.pen.color(), t->opacity));
                // 统一 Qt 语义的文本格式 (见 jzTextFormat): 与测量侧严格一致, 空格照常绘制。
                std::unique_ptr<StringFormat> fmt = jzTextFormat();
                g.DrawString(wtext.c_str(), (int)wtext.size(), font,
                             PointF((REAL)t->area.x(), (REAL)t->area.y()), fmt.get(), &br);
            }
            break;
        }
        case JZDrawTask::Image: {
            auto* t = static_cast<JZImageTask*>(task.get());
            if (t->image && t->image->pixels())
                // JZImage 走 getPixmap 转换 (窗口帧缓存; 离屏本地缓存); 几何以本地坐标提交。
                jzDrawPixmap(g, getPixmap(t->image), toGdiRect(t->area));
            break;
        }
        case JZDrawTask::Pixmap: {
            auto* t = static_cast<JZPixmapTask*>(task.get());
            jzDrawPixmap(g, t->pixmap, toGdiRect(t->area));
            break;
        }
        case JZDrawTask::Path: {
            auto* t = static_cast<JZPathTask*>(task.get());
            GraphicsPath path;
            double curX = 0, curY = 0;
            for (const auto& e : t->path.elements()) {
                switch (e.type) {
                case JZPainterPath::Move:
                    curX = e.d[0]; curY = e.d[1];
                    path.StartFigure();
                    break;
                case JZPainterPath::Line: {
                    float x1 = (float)curX, y1 = (float)curY;
                    float x2 = (float)e.d[0], y2 = (float)e.d[1];
                    path.AddLine(PointF(x1, y1), PointF(x2, y2));
                    curX = e.d[0]; curY = e.d[1];
                    break;
                }
                case JZPainterPath::Cubic: {
                    float x0 = (float)curX, y0 = (float)curY;
                    float c1x = (float)e.d[0], c1y = (float)e.d[1];
                    float c2x = (float)e.d[2], c2y = (float)e.d[3];
                    float x2 = (float)e.d[4], y2 = (float)e.d[5];
                    path.AddBezier(PointF(x0, y0), PointF(c1x, c1y),
                                   PointF(c2x, c2y), PointF(x2, y2));
                    curX = e.d[4]; curY = e.d[5];
                    break;
                }
                case JZPainterPath::Quad: {
                    // 二次贝塞尔 -> 三次贝塞尔: 控制点 c1=cur+2/3*(ctrl-cur), c2=end+2/3*(ctrl-end)
                    float x0 = (float)curX, y0 = (float)curY;
                    float cxs = (float)e.d[0], cys = (float)e.d[1];
                    float x2 = (float)e.d[2], y2 = (float)e.d[3];
                    float c1x = x0 + 2.0f / 3 * (cxs - x0);
                    float c1y = y0 + 2.0f / 3 * (cys - y0);
                    float c2x = x2 + 2.0f / 3 * (cxs - x2);
                    float c2y = y2 + 2.0f / 3 * (cys - y2);
                    path.AddBezier(PointF(x0, y0), PointF(c1x, c1y),
                                   PointF(c2x, c2y), PointF(x2, y2));
                    curX = e.d[2]; curY = e.d[3];
                    break;
                }
                case JZPainterPath::Close:
                    path.CloseFigure();
                    break;
                }
            }
            if (t->fill) {
                Color fc = t->fillColor.isSome()
                    ? toGdiColor(t->fillColor.value(), t->opacity)
                    : toGdiColor(cur.brush.color(), t->opacity);
                SolidBrush br(fc);
                g.FillPath(&br, &path);
            }
            if (t->stroke) {
                float pw = (cur.pen.width() > 0 ? cur.pen.width() : 1);
                Pen pen(toGdiColor(cur.pen.color(), t->opacity), pw);
                g.DrawPath(&pen, &path);
            }
            break;
        }
        case JZDrawTask::None:
        default:
            break;
        }
    }
}

void JZGdiBackend::replayTasks(void* graphics, JZLayer* layer)
{
    Graphics& g = *(Graphics*)graphics;
    const float s = m_widgetDevice ? m_widgetDevice->scale() : 1.0f;
    auto getPixmap = [this](const std::shared_ptr<JZImageData>& img) {
        return imagePixmap(img);
    };
    jzReplayLayer(g, layer, s, getPixmap);
}

bool JZGdiBackend::ensureGdiplus()
{
    return gdiplusLifetime().acquire();
}

void JZGdiBackend::releaseGdiplus()
{
    gdiplusLifetime().release();
}

bool JZGdiBackend::renderLayerToImage(JZImage& image, JZLayer* layer, float scale)
{
    if (!layer || image.isNull() || !gdiplusLifetime().isReady())
        return false;

    const int w = image.width();
    const int h = image.height();
    if (w <= 0 || h <= 0)
        return false;

    // 在 GDI+ 位图上合成: 先把现有像素 (RGBA8888 直 alpha) 拷入 32bppARGB,
    // 后续绘制以 SourceOver 叠加到原图之上 (与 QPainter 作用于 QImage 语义一致)。
    Bitmap bmp(w, h, PixelFormat32bppARGB);
    {
        Rect rc(0, 0, w, h);
        BitmapData bd;
        if (bmp.LockBits(&rc, ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Ok)
            return false;
        const uint8_t* src = image.data()->pixels();
        uint8_t* dst = (uint8_t*)bd.Scan0;
        const int stride = bd.Stride;
        for (int y = 0; y < h; ++y) {
            const uint8_t* s = src + (size_t)y * w * 4;
            uint8_t* d = dst + (size_t)y * stride;
            for (int x = 0; x < w; ++x) {
                d[0] = s[2];   // B
                d[1] = s[1];   // G
                d[2] = s[0];   // R
                d[3] = s[3];   // A
                s += 4; d += 4;
            }
        }
        bmp.UnlockBits(&bd);
    }

    // 离屏绘制不共享帧缓存: 同一次 paint 内反复贴同一张图时, 本地缓存其 pixmap。
    std::map<const JZImageData*, std::shared_ptr<JZPixmapData>> localCache;
    auto getPixmap = [&](const std::shared_ptr<JZImageData>& img) -> std::shared_ptr<JZPixmapData> {
        if (!img)
            return nullptr;
        auto it = localCache.find(img.get());
        if (it != localCache.end())
            return it->second;
        JZPixmap pm = JZPixmap::fromImage(JZImage::fromData(img));
        std::shared_ptr<JZPixmapData> d = pm.isNull() ? nullptr : pm.data();
        localCache[img.get()] = d;
        return d;
    };

    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.SetCompositingQuality(CompositingQualityHighSpeed);
        jzReplayLayer(g, layer, scale, getPixmap);
    }

    // 读回: 32bppARGB (B,G,R,A) -> RGBA8888 直 alpha, 写回 JZImage 像素缓冲。
    {
        Rect rc(0, 0, w, h);
        BitmapData bd;
        if (bmp.LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok)
            return false;
        uint8_t* dst = image.data()->pixels();
        const uint8_t* src = (const uint8_t*)bd.Scan0;
        const int stride = bd.Stride;
        for (int y = 0; y < h; ++y) {
            const uint8_t* s = src + (size_t)y * stride;
            uint8_t* d = dst + (size_t)y * w * 4;
            for (int x = 0; x < w; ++x) {
                d[0] = s[2];   // R
                d[1] = s[1];   // G
                d[2] = s[0];   // B
                d[3] = s[3];   // A
                s += 4; d += 4;
            }
        }
        bmp.UnlockBits(&bd);
    }
    return true;
}

// JZImageTask 的懒转换: JZImageData -> JZPixmap, 每张图只转一次并缓存。
// 转换与像素布局全部由 JZPixmap::fromImage 负责 (那里是一次性 O(像素) 的开销,
// 4K 约 40ms); 这里只做"按源图地址查缓存"。缓存不持有源图 (weak_ptr), 源图一
// 销毁条目就成过期状态, 下一帧 purgeImageCache() 回收那几十 MB 的 pixmap。
std::shared_ptr<JZPixmapData> JZGdiBackend::imagePixmap(const std::shared_ptr<JZImageData>& img)
{
    for (auto& e : m_images) {
        if (std::shared_ptr<JZImageData> alive = e.image.lock())
            if (alive.get() == img.get())
                return e.pixmap;
    }

    const JZPixmap pm = JZPixmap::fromImage(JZImage::fromData(img));
    if (pm.isNull())
        return nullptr;

    ImageEntry e;
    e.image = img;
    e.pixmap = pm.data();
    m_images.push_back(e);
    return e.pixmap;
}

// 源图已无人持有 (标注窗关闭后那张全屏截图就是几十 MB) 的 pixmap 回收。
// 每帧只留当前真正被绘制的图, 缓存不会随截图次数无限涨。
void JZGdiBackend::purgeImageCache()
{
    if (m_images.empty())
        return;
    std::vector<ImageEntry> keep;
    keep.reserve(m_images.size());
    for (auto& e : m_images) {
        if (!e.image.expired())
            keep.push_back(e);
    }
    m_images.swap(keep);
}
