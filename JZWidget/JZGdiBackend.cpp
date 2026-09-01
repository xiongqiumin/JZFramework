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

#include <string>
#include <vector>

using namespace Gdiplus;

// ==================== GDI+ 进程级生命周期 (多窗口共享, 引用计数) ====================
namespace {

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

    void release()
    {
        if (refCount > 0 && --refCount == 0) {
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

// 文本字体族 (优先微软雅黑, 失败回退)
static const FontFamily& jzFontFamily()
{
    static FontFamily family(L"Microsoft YaHei");
    return family;
}

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

    int textWidth(const std::string& text, int pixelSize) override
    {
        std::wstring wtext = utf8ToUtf16(text);
        if (wtext.empty())
            return 0;

        Font font(probeFamily(pixelSize), (REAL)pixelSize, FontStyleRegular, UnitPixel);
        RectF layout(0.0f, 0.0f, 1000000.0f, (REAL)(pixelSize * 4));
        RectF bounds;
        // GenericTypographic: 紧贴字形无外边距, 适合做居中对齐
        m_graphics->MeasureString(wtext.c_str(), (INT)wtext.size(), &font, layout,
                                  StringFormat::GenericTypographic(), &bounds);
        return (int)(bounds.Width + 0.5f);
    }

    int textHeight(int pixelSize) override
    {
        Font font(probeFamily(pixelSize), (REAL)pixelSize, FontStyleRegular, UnitPixel);
        return (int)(font.GetHeight(m_graphics) + 0.5f);
    }

    // 以下三个供文本控件做光标定位与垂直居中:
    // GDI+ 的 CellAscent/CellDescent/LineSpacing 是"设计单位"(以 em 为单位),
    // 需按 emHeight 换算到像素; LineSpacing - Ascent - Descent 即 internalLeading
    // (位于字符 cell 顶部、字形之上的空白), 不扣掉它文字会视觉偏低。
    int ascent(int pixelSize) override
    {
        return designToPixel(probeFamily(pixelSize)->GetCellAscent(FontStyleRegular), pixelSize);
    }

    int descent(int pixelSize) override
    {
        return designToPixel(probeFamily(pixelSize)->GetCellDescent(FontStyleRegular), pixelSize);
    }

    int internalLeading(int pixelSize) override
    {
        const FontFamily* fam = probeFamily(pixelSize);
        int spacing = designToPixel(fam->GetLineSpacing(FontStyleRegular), pixelSize);
        int rest = spacing - ascent(pixelSize) - descent(pixelSize);
        return (rest > 0) ? rest : 0;
    }

private:
    // 设计单位 -> 像素
    int designToPixel(UINT16 design, int pixelSize)
    {
        UINT16 em = probeFamily(pixelSize)->GetEmHeight(FontStyleRegular);
        if (em == 0)
            return 0;
        return (int)((float)design * pixelSize / (float)em + 0.5f);
    }

    const FontFamily* probeFamily(int pixelSize)
    {
        Font probe(&jzFontFamily(), (REAL)pixelSize, FontStyleRegular, UnitPixel);
        return (probe.GetLastStatus() == Ok) ? &jzFontFamily()
                                             : FontFamily::GenericSansSerif();
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

bool JZGdiBackend::init(JZDisplayDevice* device, void* hwnd, unsigned physicalW, unsigned physicalH)
{
    m_widgetDevice = device;
    if (!hwnd || !gdiplusLifetime().acquire())
        return false;

    // 字体度量引擎 (进程级, 引用计数与 GDI+ 生命周期一致)
    if (fontEngineRefCount()++ == 0) {
        s_fontEngine = new GdiFontEngine();
        JZFontMetrics::setEngine(s_fontEngine);
    }

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

    // 字体度量引擎先于 GdiplusShutdown 注销
    if (fontEngineRefCount() > 0 && --fontEngineRefCount() == 0) {
        JZFontMetrics::setEngine(nullptr);
        delete s_fontEngine;
        s_fontEngine = nullptr;
    }

    gdiplusLifetime().release();
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
            g.SetClip(Rect((INT)c.X, (INT)c.Y, (INT)(c.Width + 0.5f), (INT)(c.Height + 0.5f)));
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
            RectF r = toGdiRect(t->area, s);
            float w = s * (cur.pen.width() > 0 ? cur.pen.width() : 1);
            Pen pen(toGdiColor(cur.pen.color(), t->opacity), w);
            if (t->radius > 0) {
                GraphicsPath p;
                addRoundRectPath(p, r, t->radius * s);
                g.DrawPath(&pen, &p);
            } else {
                g.DrawRectangle(&pen, r);
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
                // Font 禁止赋值, 先探测可用字体族, 再一次性构造
                const FontFamily* family = &jzFontFamily();
                {
                    Font probe(family, cur.font.size() * s, FontStyleRegular, UnitPixel);
                    if (probe.GetLastStatus() != Ok)
                        family = FontFamily::GenericSansSerif();
                }
                Font font(family, cur.font.size() * s, FontStyleRegular, UnitPixel);
                SolidBrush br(toGdiColor(cur.pen.color(), t->opacity));
                g.DrawString(wtext.c_str(), (int)wtext.size(), &font,
                             PointF(t->area.x() * s, t->area.y() * s), &br);
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
