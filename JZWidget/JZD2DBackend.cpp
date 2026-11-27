#include "JZD2DBackend.h"
#include "JZDisplayDevice.h"
#include "JZLayer.h"
#include "JZDrawTask.h"
#include "JZPixmap.h"
#include "JZImage.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX          // 否则 windows.h 的 min/max 宏会把 std::min/std::max 顶掉
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// ============================================================================
// 为什么 D2D 而不是 OpenGL (2026-11-27 决策)
//
// 1) 图元形状完全对口: 绘制流只有 Rect/RoundRect/Border/Line/Arc/Path/Text/Pixmap,
//    外加"仿射变换 + 轴对齐裁剪"。这些是 D2D 的原生 API (FillRectangle / DrawRoundedRectangle
//    / DrawLine / PathGeometry / DrawText / DrawBitmap)。换 OpenGL 要自己写: 三角化圆角与
//    贝塞尔、线宽/接头的几何展开、字形图集与排版 (或再挂一层 FreeType+ 图集)、以及所有
//    抗锯齿 AA。工作量与维护成本高一个数量级, 收益只是"跨平台"——而本项目 UI 只有 Windows。
//
// 2) 像素格式天然一致: JZPixmap 是 32bppPARGB (内存序 B,G,R,A, 已预乘), D2D 的
//    B8G8R8A8_UNORM + PREMULTIPLIED 就是它。贴图 = CreateBitmap(指针直接给), 零转换;
//    OpenGL 要 RGBA + 非预乘 (或自己在 shader 里预乘), 每张图都要转一次。
//
// 3) 窗口与呈现: D2D 的 HwndRenderTarget 内部就是 D3D11+DXGI 交换链, 由系统管 resize/
//    设备丢失/DWM 合成; OpenGL 在 Win32 上要自建 WGL 上下文 + 双缓冲 + vsync 策略, 还得
//    和现有 GDI 双缓冲路径 (JZWin32WindowBackend 的 DIB/BitBlt) 互斥切换, 窗口子控件
//    自绘 (整窗一块 BitBlt) 的模型也得改。
//
// 4) 退化路径: 远程桌面/虚拟机里 D2D 自动落到 WARP (软件 D3D), 不需要另写后端;
//    OpenGL 在 RDP 下常常直接是 1.1 软件实现甚至没有。
//
// 5) 与现有离屏/虚拟桌面不冲突: 那条路要 targetPixels 零拷贝取 CPU 帧, 是"CPU 缓冲"的
//    形状 —— 本后端只负责窗口呈现, init(target==nullptr) 返回 false, 设备自动回退 GDI。
// ============================================================================

namespace {

std::wstring toWide(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

D2D1_COLOR_F toD2DColor(const JZColor& c, uint8_t opacity)
{
    const float a = (float)c.alpha() / 255.0f * ((float)opacity / 255.0f);
    return D2D1::ColorF(c.red() / 255.0f, c.green() / 255.0f, c.blue() / 255.0f, a);
}

D2D1_RECT_F toD2DRect(const JZRect& r)
{
    return D2D1::RectF((float)r.x(), (float)r.y(),
                       (float)(r.x() + r.width()), (float)(r.y() + r.height()));
}

D2D1_MATRIX_3X2_F toD2DMatrix(const JZMatrix2D& m)
{
    // JZMatrix2D 行主序: x' = m00*x + m01*y + m02。
    // D2D Matrix3x2F 的 (_11,_12,_21,_22,_31,_32) 对应 x' = _11*x + _21*y + _31。
    return D2D1::Matrix3x2F((float)m.at(0, 0), (float)m.at(1, 0),
                            (float)m.at(0, 1), (float)m.at(1, 1),
                            (float)m.at(0, 2), (float)m.at(1, 2));
}

// 绘制流状态 (与 JZGdiBackend 的 StreamState 一一对应)
struct StreamState {
    JZPen pen;
    JZBrush brush;
    JZFont font;
    JZRect clip;
    JZMatrix2D transform;
    int ox = 0;
    int oy = 0;
};

// JZPainterPath -> D2D 路径几何 (Move/Line/Cubic/Quad/Close)
HRESULT buildGeometry(ID2D1Factory* factory, const JZPainterPath& src,
                      ID2D1PathGeometry** out)
{
    ComPtr<ID2D1PathGeometry> geo;
    HRESULT hr = factory->CreatePathGeometry(&geo);
    if (FAILED(hr))
        return hr;

    ComPtr<ID2D1GeometrySink> sink;
    hr = geo->Open(&sink);
    if (FAILED(hr))
        return hr;

    double curX = 0, curY = 0;
    bool open = false;
    for (const auto& e : src.elements()) {
        switch (e.type) {
        case JZPainterPath::Move:
            if (open)
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
            curX = e.d[0];
            curY = e.d[1];
            sink->BeginFigure(D2D1::Point2F((float)curX, (float)curY),
                              D2D1_FIGURE_BEGIN_FILLED);
            open = true;
            break;
        case JZPainterPath::Line:
            if (!open) {
                sink->BeginFigure(D2D1::Point2F((float)curX, (float)curY),
                                  D2D1_FIGURE_BEGIN_FILLED);
                open = true;
            }
            sink->AddLine(D2D1::Point2F((float)e.d[0], (float)e.d[1]));
            curX = e.d[0];
            curY = e.d[1];
            break;
        case JZPainterPath::Cubic:
            if (!open) {
                sink->BeginFigure(D2D1::Point2F((float)curX, (float)curY),
                                  D2D1_FIGURE_BEGIN_FILLED);
                open = true;
            }
            sink->AddBezier(D2D1::BezierSegment(
                D2D1::Point2F((float)e.d[0], (float)e.d[1]),
                D2D1::Point2F((float)e.d[2], (float)e.d[3]),
                D2D1::Point2F((float)e.d[4], (float)e.d[5])));
            curX = e.d[4];
            curY = e.d[5];
            break;
        case JZPainterPath::Quad: {
            if (!open) {
                sink->BeginFigure(D2D1::Point2F((float)curX, (float)curY),
                                  D2D1_FIGURE_BEGIN_FILLED);
                open = true;
            }
            // 二次 -> 三次 (与 GDI+ 后端同一套换算)
            const float x0 = (float)curX, y0 = (float)curY;
            const float cxs = (float)e.d[0], cys = (float)e.d[1];
            const float x2 = (float)e.d[2], y2 = (float)e.d[3];
            sink->AddBezier(D2D1::BezierSegment(
                D2D1::Point2F(x0 + 2.0f / 3 * (cxs - x0), y0 + 2.0f / 3 * (cys - y0)),
                D2D1::Point2F(x2 + 2.0f / 3 * (cxs - x2), y2 + 2.0f / 3 * (cys - y2)),
                D2D1::Point2F(x2, y2)));
            curX = e.d[2];
            curY = e.d[3];
            break;
        }
        case JZPainterPath::Close:
            if (open) {
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                open = false;
            }
            break;
        }
    }
    if (open)
        sink->EndFigure(D2D1_FIGURE_END_OPEN);

    hr = sink->Close();
    if (FAILED(hr))
        return hr;
    *out = geo.Detach();
    return S_OK;
}

} // namespace

// ============================================================================
// Impl
// ============================================================================
struct JZD2DBackend::Impl
{
    ComPtr<ID2D1Factory> factory;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1HwndRenderTarget> rt;

    HWND hwnd = nullptr;
    unsigned physW = 0;
    unsigned physH = 0;
    JZDisplayDevice* device = nullptr;
    bool ready = false;
    bool clipPushed = false;
    float dpi = 96.0f;            // 当前已应用到 rt 的 DPI (含 DPI 缩放)

    struct TextEntry {
        std::string key;
        ComPtr<IDWriteTextFormat> fmt;
    };
    struct BitmapEntry {
        const JZPixmapData* key = nullptr;
        std::shared_ptr<JZPixmapData> hold;   // 防止像素缓冲被回收
        ComPtr<ID2D1Bitmap> bmp;
    };
    struct ImageEntry {
        std::weak_ptr<JZImageData> src;
        std::shared_ptr<JZPixmapData> pm;
    };
    std::vector<TextEntry> texts;
    std::vector<BitmapEntry> bitmaps;
    std::vector<ImageEntry> images;

    IDWriteTextFormat* textFormat(const JZFont& f)
    {
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s|%d|%d",
                    f.family().empty() ? "" : f.family().c_str(), f.size(), f.style());
        const std::string key(buf);
        for (auto& e : texts)
            if (e.key == key)
                return e.fmt.Get();

        std::wstring family = toWide(f.family());
        const wchar_t* fam = family.empty() ? L"Microsoft YaHei" : family.c_str();

        DWRITE_FONT_WEIGHT weight = f.bold() ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
        DWRITE_FONT_STYLE style = f.italic() ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
        const float size = (float)(f.size() > 0 ? f.size() : 12);

        ComPtr<IDWriteTextFormat> fmt;
        if (FAILED(dwrite->CreateTextFormat(fam, nullptr, weight, style,
                                            DWRITE_FONT_STRETCH_NORMAL, size, L"", &fmt)))
            return nullptr;
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        TextEntry e;
        e.key = key;
        e.fmt = fmt;
        texts.push_back(e);
        return texts.back().fmt.Get();
    }

    ID2D1Bitmap* bitmapFor(const std::shared_ptr<JZPixmapData>& pm)
    {
        if (!pm || !pm->pixels() || pm->width() <= 0 || pm->height() <= 0)
            return nullptr;

        // 线性查找: 同时在用的位图通常只有几张 (图标/纹理), 比哈希表更省
        for (auto& e : bitmaps) {
            if (e.key == pm.get() && e.hold.get() == pm.get())
                return e.bmp.Get();
        }

        const D2D1_SIZE_U size = D2D1::SizeU((UINT32)pm->width(), (UINT32)pm->height());
        const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);      // 位图 DPI 只影响 DrawBitmap 的"默认尺寸", 我们总是给目标矩形

        ComPtr<ID2D1Bitmap> bmp;
        if (FAILED(rt->CreateBitmap(size, pm->pixels(), (UINT32)(pm->width() * 4), &props, &bmp)))
            return nullptr;

        BitmapEntry e;
        e.key = pm.get();
        e.hold = pm;
        e.bmp = bmp;
        bitmaps.push_back(e);

        // 简单的容量上限: 位图缓存持有 CPU 侧 pixmap, 不设上限会随截图/图片流无限涨
        if (bitmaps.size() > 256) {
            bitmaps.erase(bitmaps.begin(), bitmaps.begin() + 64);
        }
        return bitmaps.back().bmp.Get();
    }

    std::shared_ptr<JZPixmapData> imagePixmap(const std::shared_ptr<JZImageData>& img)
    {
        for (auto& e : images) {
            if (std::shared_ptr<JZImageData> alive = e.src.lock())
                if (alive.get() == img.get())
                    return e.pm;
        }
        const JZPixmap pm = JZPixmap::fromImage(JZImage::fromData(img));
        if (pm.isNull())
            return nullptr;
        ImageEntry e;
        e.src = img;
        e.pm = pm.data();
        images.push_back(e);
        return e.pm;
    }

    void purgeImages()
    {
        if (images.empty())
            return;
        std::vector<ImageEntry> keep;
        keep.reserve(images.size());
        for (auto& e : images)
            if (!e.src.expired())
                keep.push_back(e);
        images.swap(keep);
    }

    void release()
    {
        texts.clear();
        bitmaps.clear();
        images.clear();
        rt.Reset();
        dwrite.Reset();
        factory.Reset();
        ready = false;
        clipPushed = false;
        physW = physH = 0;
        hwnd = nullptr;
    }
};

// ============================================================================
// 生命周期
// ============================================================================
JZD2DBackend::JZD2DBackend()
    : m_d(new Impl)
{
}

JZD2DBackend::~JZD2DBackend()
{
    destroy();
}

bool JZD2DBackend::available()
{
    static int cached = -1;      // -1 未知, 0 不可用, 1 可用
    if (cached < 0) {
        ComPtr<ID2D1Factory> f;
        cached = SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                             __uuidof(ID2D1Factory), nullptr,
                                             reinterpret_cast<void**>(f.GetAddressOf()))) ? 1 : 0;
    }
    return cached == 1;
}

bool JZD2DBackend::init(JZDisplayDevice* device, void* target,
                        unsigned physicalW, unsigned physicalH)
{
    if (!target || physicalW == 0 || physicalH == 0)
        return false;      // 离屏: 不支持 (设备会回退 GDI, 那里有 targetPixels 零拷贝取帧)

    // 必须是**真实** Win32 窗口: 虚拟桌面的窗口后端给的是自己的窗口对象 (不是 HWND),
    // 拿它去建 HwndRenderTarget 只会在渲染期静默失败 —— 提前拒绝, 让设备回退 GDI。
    if (!IsWindow((HWND)target))
        return false;

    Impl& d = *m_d;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
                                 nullptr, reinterpret_cast<void**>(d.factory.GetAddressOf()))))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(d.dwrite.GetAddressOf()))))
        return false;

    d.device = device;
    d.hwnd = (HWND)target;
    d.physW = physicalW;
    d.physH = physicalH;
    d.ready = true;
    return true;
}

void JZD2DBackend::destroy()
{
    if (m_d)
        m_d->release();
}

void JZD2DBackend::resizeSwapChain(unsigned physicalW, unsigned physicalH)
{
    Impl& d = *m_d;
    if (!d.ready || physicalW == 0 || physicalH == 0)
        return;
    d.physW = physicalW;
    d.physH = physicalH;
    if (d.rt) {
        if (FAILED(d.rt->Resize(D2D1::SizeU(physicalW, physicalH))))
            d.rt.Reset();     // 设备丢了: 下一帧重建
    }
}

void JZD2DBackend::rebuildFonts()
{
    if (m_d)
        m_d->texts.clear();   // 字号是 DIP, 跟着 DPI 走; 这里只是清掉旧缓存
}

bool JZD2DBackend::snapshot(JZImage& out)
{
    (void)out;
    return false;             // 帧缓冲在 GPU 上
}

// ============================================================================
// 回放
// ============================================================================
void JZD2DBackend::render(JZLayer* layer)
{
    if (!layer)
        return;
    Impl& d = *m_d;
    if (!d.ready) {
        layer->clearTasks();
        return;
    }

    // 设备目标懒创建 (init 后第一次 render / resize 后 / 设备丢失后)
    if (!d.rt) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(
            d.hwnd, D2D1::SizeU(d.physW, d.physH), D2D1_PRESENT_OPTIONS_NONE);
        if (FAILED(d.factory->CreateHwndRenderTarget(props, hwndProps, d.rt.GetAddressOf()))) {
            layer->clearTasks();
            return;           // 创建失败: 本帧不画 (下一帧再试)
        }
    }

    d.purgeImages();

    // DPI = 96 * 控件缩放: 于是"逻辑像素 == DIP", 变换里不再需要 Scale(s)。
    // 好处: 文本按物理分辨率栅格化 (不糊), 圆角/笔宽也按真实 DPI 计算。
    const float s = d.device ? (float)d.device->scale() : 1.0f;
    const float dpi = 96.0f * (s > 0.0f ? s : 1.0f);
    if (std::fabs(dpi - d.dpi) > 0.01f) {
        d.rt->SetDpi(dpi, dpi);
        d.dpi = dpi;
    }

    d.rt->BeginDraw();
    d.rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    d.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    d.rt->SetTransform(D2D1::Matrix3x2F::Identity());

    replay(layer);

    if (d.clipPushed) {
        d.rt->PopAxisAlignedClip();
        d.clipPushed = false;
    }

    const HRESULT hr = d.rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        d.rt.Reset();         // 下一帧重建
        d.texts.clear();
    }

    layer->clearTasks();
}

void JZD2DBackend::replay(JZLayer* layer)
{
    Impl& d = *m_d;
    ID2D1HwndRenderTarget* rt = d.rt.Get();
    ID2D1Factory* factory = d.factory.Get();
    if (!rt || !factory)
        return;

    std::vector<StreamState> stack;
    StreamState cur;
    std::vector<ComPtr<ID2D1SolidColorBrush>> brushPool;   // 保持本帧刷子存活

    auto makeBrush = [&](const JZColor& c, uint8_t opacity) -> ID2D1SolidColorBrush* {
        ComPtr<ID2D1SolidColorBrush> br;
        if (FAILED(rt->CreateSolidColorBrush(toD2DColor(c, opacity), &br)))
            return nullptr;
        brushPool.push_back(br);
        return br.Get();
    };

    // 把当前状态施加到 rt: 世界变换 + 显示坐标裁剪。
    // 裁剪与 GDI+ 语义一致是**显示坐标的轴对齐矩形** —— 所以先在 identity 下 Push,
    // 再设回世界变换 (D2D 在 Push 时就按当时的变换固化了裁剪区域)。
    auto apply = [&](const StreamState& st) {
        if (d.clipPushed) {
            rt->PopAxisAlignedClip();
            d.clipPushed = false;
        }
        const JZMatrix2D world = JZMatrix2D::translation(st.ox, st.oy).multiplied(st.transform);
        if (!st.clip.isEmpty()) {
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
            const float x0 = (float)st.clip.x();
            const float y0 = (float)st.clip.y();
            const float x1 = (float)(st.clip.x() + st.clip.width());
            const float y1 = (float)(st.clip.y() + st.clip.height());
            rt->PushAxisAlignedClip(D2D1::RectF(x0, y0, x1, y1),
                                    D2D1_ANTIALIAS_MODE_ALIASED);
            d.clipPushed = true;
        }
        rt->SetTransform(toD2DMatrix(world));
    };

    apply(cur);

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
            apply(cur);
            break;
        }
        case JZDrawTask::Pop: {
            if (!stack.empty()) {
                cur = stack.back();
                stack.pop_back();
            } else {
                cur = StreamState();
            }
            apply(cur);
            break;
        }
        case JZDrawTask::Change: {
            auto* t = static_cast<JZDrawChangeTask*>(task.get());
            if (t->pen.isSome())    cur.pen = t->pen.value();
            if (t->brush.isSome())  cur.brush = t->brush.value();
            if (t->font.isSome())   cur.font = t->font.value();
            if (t->clip.isSome())   cur.clip = t->clip.value();
            if (t->matrix.isSome()) cur.transform = t->matrix.value();
            apply(cur);
            break;
        }
        case JZDrawTask::Line: {
            auto* t = static_cast<JZLineTask*>(task.get());
            ID2D1SolidColorBrush* br = makeBrush(cur.pen.color(), t->opacity);
            if (!br)
                break;
            const float w = (float)(cur.pen.width() > 0 ? cur.pen.width() : 1);
            rt->DrawLine(D2D1::Point2F((float)t->x1, (float)t->y1),
                         D2D1::Point2F((float)t->x2, (float)t->y2), br, w);
            break;
        }
        case JZDrawTask::Rect: {
            auto* t = static_cast<JZFillRectTask*>(task.get());
            ID2D1SolidColorBrush* br = makeBrush(t->color, t->opacity);
            if (!br)
                break;
            const D2D1_RECT_F rc = toD2DRect(t->area);
            if (t->radius > 0) {
                const float r = (float)std::min(t->radius,
                    (int)std::min(t->area.width(), t->area.height()) / 2);
                rt->FillRoundedRectangle(D2D1::RoundedRect(rc, r, r), br);
            } else {
                rt->FillRectangle(rc, br);
            }
            break;
        }
        case JZDrawTask::Border: {
            auto* t = static_cast<JZBorderTask*>(task.get());
            float pw = (float)(cur.pen.width() > 0 ? cur.pen.width() : 1);
            if (pw <= 0.0f)
                break;
            ID2D1SolidColorBrush* br = makeBrush(cur.pen.color(), t->opacity);
            if (!br)
                break;

            // 与 GDI+ 一致: 向内缩半笔宽 (GDI+ 的笔是沿路径居中, 内缩后外缘恰好贴合矩形)
            float half = pw * 0.5f;
            half = std::min(half, std::min((float)t->area.width(), (float)t->area.height()) * 0.5f);
            const D2D1_RECT_F rc = D2D1::RectF(
                (float)t->area.x() + half, (float)t->area.y() + half,
                (float)(t->area.x() + t->area.width()) - half,
                (float)(t->area.y() + t->area.height()) - half);
            if (t->radius > 0) {
                const float r = std::max(0.0f, (float)t->radius - half);
                rt->DrawRoundedRectangle(D2D1::RoundedRect(rc, r, r), br, pw);
            } else {
                rt->DrawRectangle(rc, br, pw);
            }
            break;
        }
        case JZDrawTask::Arc: {
            auto* t = static_cast<JZArcTask*>(task.get());
            const float radius = (float)(t->arcRadius > 0 ? t->arcRadius : 1);
            float sweep = (float)(t->arcEndAngle - t->arcStartAngle) / 10.0f;
            if (sweep < 0.0f)
                sweep += 360.0f;
            if (sweep <= 0.0f)
                break;
            ID2D1SolidColorBrush* br = makeBrush(cur.pen.color(), t->opacity);
            if (!br)
                break;
            const float w = (float)(cur.pen.width() > 0 ? cur.pen.width() : 1);

            if (sweep >= 359.5f) {
                rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((float)t->arcCx, (float)t->arcCy),
                                              radius, radius), br, w);
                break;
            }

            const double pi = 3.14159265358979323846;
            const double a0 = (double)t->arcStartAngle / 10.0 * pi / 180.0;
            const double a1 = a0 + (double)sweep * pi / 180.0;
            const D2D1_POINT_2F p0 = D2D1::Point2F(
                (float)(t->arcCx + radius * std::cos(a0)),
                (float)(t->arcCy + radius * std::sin(a0)));
            const D2D1_POINT_2F p1 = D2D1::Point2F(
                (float)(t->arcCx + radius * std::cos(a1)),
                (float)(t->arcCy + radius * std::sin(a1)));

            ComPtr<ID2D1PathGeometry> geo;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(factory->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink)))
                break;
            sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(
                p1, D2D1::SizeF(radius, radius), 0.0f,
                D2D1_SWEEP_DIRECTION_CLOCKWISE,
                sweep > 180.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (SUCCEEDED(sink->Close()))
                rt->DrawGeometry(geo.Get(), br, w);
            break;
        }
        case JZDrawTask::Text: {
            auto* t = static_cast<JZTextTask*>(task.get());
            if (t->text.empty())
                break;
            IDWriteTextFormat* fmt = d.textFormat(cur.font);
            ID2D1SolidColorBrush* br = makeBrush(cur.pen.color(), t->opacity);
            if (!fmt || !br)
                break;
            const std::wstring w = toWide(t->text);
            if (w.empty())
                break;
            // 目标矩形给足空间: TextFormat 已是 NOWRAP + 左上对齐, 与原 DrawString 语义一致
            const D2D1_RECT_F rc = D2D1::RectF((float)t->area.x(), (float)t->area.y(),
                                               (float)t->area.x() + 8192.0f,
                                               (float)t->area.y() + 8192.0f);
            rt->DrawText(w.c_str(), (UINT32)w.size(), fmt, rc, br,
                         D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
            break;
        }
        case JZDrawTask::Image: {
            auto* t = static_cast<JZImageTask*>(task.get());
            if (!t->image)
                break;
            ID2D1Bitmap* bmp = d.bitmapFor(d.imagePixmap(t->image));
            if (bmp)
                rt->DrawBitmap(bmp, toD2DRect(t->area), 1.0f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            break;
        }
        case JZDrawTask::Pixmap: {
            auto* t = static_cast<JZPixmapTask*>(task.get());
            ID2D1Bitmap* bmp = d.bitmapFor(t->pixmap);
            if (bmp)
                rt->DrawBitmap(bmp, toD2DRect(t->area), 1.0f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            break;
        }
        case JZDrawTask::Path: {
            auto* t = static_cast<JZPathTask*>(task.get());
            if (t->path.isEmpty())
                break;

            // 先平移 (ox,oy)? 不 —— 路径与其它图元一样是**本地坐标**, 由世界变换统一施加
            ComPtr<ID2D1PathGeometry> geo;
            if (FAILED(buildGeometry(factory, t->path, geo.GetAddressOf())))
                break;

            if (t->fill) {
                const JZColor fc = t->fillColor.isSome() ? t->fillColor.value() : cur.brush.color();
                ID2D1SolidColorBrush* br = makeBrush(fc, t->opacity);
                if (br)
                    rt->FillGeometry(geo.Get(), br);
            }
            if (t->stroke) {
                ID2D1SolidColorBrush* br = makeBrush(cur.pen.color(), t->opacity);
                if (br) {
                    const float w = (float)(cur.pen.width() > 0 ? cur.pen.width() : 1);
                    rt->DrawGeometry(geo.Get(), br, w);
                }
            }
            break;
        }
        case JZDrawTask::None:
        default:
            break;
        }
    }
}
