#include "JZGdiBackend.h"

#include "JZLayer.h"
#include "JZDrawTask.h"
#include "JZImage.h"
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

// JZRect (宽 = right - left + 1) -> GDI+ 物理像素 [x, x + w*s)
static RectF toGdiRect(const JZRect& rc, float scale)
{
    return RectF(rc.x() * scale, rc.y() * scale, rc.width() * scale, rc.height() * scale);
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
    if (!hwnd || !acquireFontEngine())
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
    // 缓存位图必须在 GdiplusShutdown 之前释放
    for (auto& kv : m_bitmaps)
        delete (Bitmap*)kv.second;
    m_bitmaps.clear();

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
    if (!m_hwnd || physicalW == 0 || physicalH == 0)
        return false;

    HDC dc = GetDC((HWND)m_hwnd);
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
    ReleaseDC((HWND)m_hwnd, dc);

    if (!m_memDC || !m_memBmp) {
        releaseMemBitmap();
        return false;
    }
    // CreateDIBSection 的新内存未初始化。整窗渲染虽会全量重画, 但逻辑 rect*scale
    // 与客户区物理尺寸存在取整差时最外圈像素可能无人绘制 —— 预填白色兜底,
    // 杜绝边缘露出未初始化内存 (黑/杂色/透明花边)。
    if (bits)
        memset(bits, 0xFF, (size_t)physicalW * physicalH * 4);
    m_oldBmp = (void*)SelectObject((HDC)m_memDC, (HBITMAP)m_memBmp);
    return true;
}

void JZGdiBackend::releaseMemBitmap()
{
    if (m_memDC && m_memBmp)
        SelectObject((HDC)m_memDC, (HBITMAP)m_oldBmp);
    if (m_memDC) { DeleteDC((HDC)m_memDC);       m_memDC = nullptr; }
    if (m_memBmp) { DeleteObject((HBITMAP)m_memBmp); m_memBmp = nullptr; }
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
    if (!m_hwnd || !m_memDC || !layer)
        return;
    if (m_physW == 0 || m_physH == 0)
        return;
    // 兜底: GDI+ 已 Shutdown 时不再绘制，否则一进 GdiPlus.dll 就崩
    if (!gdiplusLifetime().isReady())
        return;

    {
        // 任务回放到内存位图 (保留上帧内容, 脏区由各任务的 clip 覆盖)
        Graphics g((HDC)m_memDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.SetCompositingQuality(CompositingQualityHighSpeed);
        replayTasks(&g, layer);
    }   // Graphics 先析构, 再提交到屏幕

    HDC wdc = GetDC((HWND)m_hwnd);
    BitBlt(wdc, 0, 0, (int)m_physW, (int)m_physH, (HDC)m_memDC, 0, 0, SRCCOPY);
    ReleaseDC((HWND)m_hwnd, wdc);

    layer->clearTasks();
}

void JZGdiBackend::replayTasks(void* graphics, JZLayer* layer)
{
    Graphics& g = *(Graphics*)graphics;
    const float s = m_widgetDevice ? m_widgetDevice->scale() : 1.0f;

    // 绘制流状态机: 几何任务只携带 geometry, 样式 (pen/brush/font) 与裁剪 (clip)
    // 由 Push (保存当前状态并应用新状态) / Pop (回退) 维护。
    struct StreamState {
        JZPen pen;
        JZBrush brush;
        JZFont font;
        JZRect clip;
    };
    std::vector<StreamState> stack;
    StreamState cur;   // 默认: 空 clip (无裁剪) + 默认 pen/brush/font

    JZRect appliedClip;   // 当前已应用到 Graphics 的裁剪 (全局坐标)
    bool clipApplied = false;
    auto applyClip = [&](const JZRect& clip) {
        if (clipApplied && clip.x() == appliedClip.x() && clip.y() == appliedClip.y() &&
            clip.width() == appliedClip.width() && clip.height() == appliedClip.height())
            return;
        if (clipApplied && !appliedClip.isEmpty())
            g.ResetClip();
        if (!clip.isEmpty()) {
            RectF c = toGdiRect(clip, s);
            // 逻辑裁剪区 * scale 后是小数边界。直接 (INT) 截断会把落在 [截断边界,
            // 实际外缘] 的最后一条 AA 像素行整行裁掉 —— 非整数 DPI (125%/150%)
            // 下表现为控件底/右边框消失 (见描边分支注释, 笔宽*s 后外圈 AA 摊薄,
            // 又被 clip 切边则完全不显示)。
            // floor/ceil 外扩到覆盖实际几何外缘: 描边几何已内缩半笔宽, 不会越出
            // 控件物理外缘, 多放行的只可能是控件自身 AA 像素; 整数 scale 下
            // floor==trunc、ceil==(int)(x+0.5) 对整数值, 结果与原截断完全一致, 无回归。
            INT x0 = (INT)std::floor(c.X),        y0 = (INT)std::floor(c.Y);
            INT x1 = (INT)std::ceil(c.X + c.Width), y1 = (INT)std::ceil(c.Y + c.Height);
            g.SetClip(Rect(x0, y0, x1 - x0, y1 - y0));
        }
        appliedClip = clip;
        clipApplied = true;
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
            applyClip(cur.clip);
            break;
        }
        case JZDrawTask::Pop: {
            if (!stack.empty()) {
                cur = stack.back();
                stack.pop_back();
            } else {
                cur = StreamState();
            }
            applyClip(cur.clip);
            break;
        }
        case JZDrawTask::Change: {
            auto* t = static_cast<JZDrawChangeTask*>(task.get());
            if (t->pen.isSome())   cur.pen = t->pen.value();
            if (t->brush.isSome()) cur.brush = t->brush.value();
            if (t->font.isSome())  cur.font = t->font.value();
            if (t->clip.isSome()) {
                cur.clip = t->clip.value();
                applyClip(cur.clip);
            }
            break;
        }
        case JZDrawTask::Line: {
            auto* t = static_cast<JZLineTask*>(task.get());
            float w = s * (cur.pen.width() > 0 ? cur.pen.width() : 1);
            Pen pen(toGdiColor(cur.pen.color(), t->opacity), w);
            g.DrawLine(&pen, t->x1 * s, t->y1 * s, t->x2 * s, t->y2 * s);
            break;
        }
        case JZDrawTask::Rect: {
            auto* t = static_cast<JZFillRectTask*>(task.get());
            RectF r = toGdiRect(t->area, s);
            SolidBrush br(toGdiColor(t->color, t->opacity));
            if (t->radius > 0) {
                GraphicsPath p;
                addRoundRectPath(p, r, t->radius * s);
                g.FillPath(&br, &p);
            } else {
                g.FillRectangle(&br, r);
            }
            break;
        }
        case JZDrawTask::Border: {
            auto* t = static_cast<JZBorderTask*>(task.get());
            float pw = s * (cur.pen.width() > 0 ? cur.pen.width() : 1);
            if (pw <= 0.0f)
                break;
            RectF r = toGdiRect(t->area, s);
            Pen pen(toGdiColor(cur.pen.color(), t->opacity), pw);

            // 描边几何须向内缩半笔宽, 与 Qt drawRect 语义对齐:
            //   fillRect(x,y,w,h) 填充像素 [x..x+w-1]×[y..y+h-1] (RectF 外缘 = 像素右/下开口);
            //   drawRect/drawRoundRect 描这条像素矩形的外圈边框, 线条中心应落在像素格线上。
            // 若不内缩而把笔心放在控件外缘 (矩形几何边界), 则右/下侧有半笔宽越出控件,
            // 被绘制流 clip (脏区∩控件 rect) 整段裁掉 —— 表现为边框右/下缺边、四边半透明淡边。
            // 内缩半笔宽后: 1px 笔 → 中心恰在像素格线, 抗锯齿整像素满覆盖; 2px 笔 (如
            // lineEdit 焦点高亮) → 恰好铺满最外两圈像素, 四边完整且与填充外缘贴合。
            float half = pw * 0.5f;
            half = std::min(half, std::min(r.Width, r.Height) * 0.5f); // 过窄时防几何翻负
            RectF ir(r.X + half, r.Y + half,
                     r.Width - 2.0f * half, r.Height - 2.0f * half);
            if (t->radius > 0) {
                // 半径同步减 half: 内缩后圆弧圆心与 fillRect 圆角填充完全重合, 描边紧贴外缘
                GraphicsPath p;
                addRoundRectPath(p, ir, std::max(0.0f, t->radius * s - half));
                g.DrawPath(&pen, &p);
            } else {
                g.DrawRectangle(&pen, ir);
            }
            break;
        }
        case JZDrawTask::Arc: {
            auto* t = static_cast<JZArcTask*>(task.get());
            // 角度: 1/10 度, 0 = 3 点方向, 顺时针; GDI+ 屏幕坐标系 (y 向下)
            // 正角度同为顺时针, 直接换算即可。
            float radius = s * (t->arcRadius > 0 ? t->arcRadius : 1);
            float sweep = (t->arcEndAngle - t->arcStartAngle) / 10.0f;
            if (sweep < 0.0f)
                sweep += 360.0f;
            if (sweep > 0.0f && radius > 0.0f) {
                float w = s * (cur.pen.width() > 0 ? cur.pen.width() : 1);
                Pen pen(toGdiColor(cur.pen.color(), t->opacity), w);
                RectF rc(t->arcCx * s - radius, t->arcCy * s - radius, radius * 2.0f, radius * 2.0f);
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
                Font primary(family, cur.font.size() * s, (FontStyle)style, UnitPixel);
                Font fallback(&jzFontFamily(), cur.font.size() * s, FontStyleRegular, UnitPixel);
                const Font* font = (primary.GetLastStatus() == Ok) ? &primary : &fallback;
                SolidBrush br(toGdiColor(cur.pen.color(), t->opacity));
                // 统一 Qt 语义的文本格式 (见 jzTextFormat): 与测量侧
                // (GdiFontEngine::textWidth) 严格一致, 空格照常绘制。
                std::unique_ptr<StringFormat> fmt = jzTextFormat();
                g.DrawString(wtext.c_str(), (int)wtext.size(), font,
                             PointF(t->area.x() * s, t->area.y() * s), fmt.get(), &br);
            }
            break;
        }
        case JZDrawTask::Image: {
            auto* t = static_cast<JZImageTask*>(task.get());
            if (t->image && t->image->pixels()) {
                Bitmap* bmp = (Bitmap*)imageBitmap(t->image.get());
                if (bmp)
                    g.DrawImage(bmp, toGdiRect(t->area, s));
            }
            break;
        }
        case JZDrawTask::None:
        default:
            break;
        }
    }
}

// RGBA8888 -> GDI+ 32bppARGB (内存字节序 BGRA), 懒缓存
void* JZGdiBackend::imageBitmap(const JZImageData* img)
{
    auto it = m_bitmaps.find(img);
    if (it != m_bitmaps.end())
        return it->second;

    if (!img->pixels() || img->width() <= 0 || img->height() <= 0)
        return nullptr;

    int w = img->width();
    int h = img->height();
    std::vector<uint8_t> bgra((size_t)w * h * 4);
    const uint8_t* src = img->pixels();
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        bgra[i * 4 + 0] = src[i * 4 + 2];
        bgra[i * 4 + 1] = src[i * 4 + 1];
        bgra[i * 4 + 2] = src[i * 4 + 0];
        bgra[i * 4 + 3] = src[i * 4 + 3];
    }

    Bitmap* bmp = new Bitmap(w, h, w * 4, PixelFormat32bppARGB, bgra.data());
    if (bmp->GetLastStatus() != Ok) {
        delete bmp;
        return nullptr;
    }
    m_bitmaps[img] = bmp;
    return bmp;
}
