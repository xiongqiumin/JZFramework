#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "JZScreenShot.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

#include "JZBrush.h"
#include "JZDir.h"
#include "JZFile.h"
#include "JZFont.h"
#include "JZFontMetrics.h"
#include "JZKeyEvent.h"
#include "JZPainter.h"
#include "JZPen.h"
#include "JZPixmap.h"
#include "JZPushButton.h"
#include "JZScreen.h"
#include "JZWidget.h"
#include "JZWindow.h"

using namespace std;

JZImage JZScreenShot::capturePrimaryScreen()
{
    return JZScreen::primaryScreen().grabScreen();
}

JZImage JZScreenShot::captureScreen(int index)
{
    JZScreen screen(index);
    if (!screen.isValid())
        return JZImage();
    return screen.grabScreen();
}

JZImage JZScreenShot::captureWidget(JZWidget* w)
{
    return w ? w->grab() : JZImage();
}

JZImage JZScreenShot::captureVirtualDesktop(JZRect* logicalRect, float deviceScale)
{
    // 各显示器单独抓取(**物理像素**)后拼成一张图, 并统一到 deviceScale
    // (= 标注窗口所在显示器的缩放, 即窗口把逻辑坐标放大成设备像素的那个倍数):
    //   画布尺寸 = 逻辑桌面尺寸 * deviceScale
    //   每屏抓图 = 该屏逻辑尺寸 * deviceScale
    // 单屏 (或各屏缩放一致) 时每屏都恰好 1:1, 完全不重采样; 标注窗口按逻辑坐标
    // 铺满桌面、绘制时乘 deviceScale -> 贴图同样是 1:1, 文字边缘锐利。
    // 旧实现统一缩到**逻辑**尺寸, 显示时再被放大 deviceScale 倍 —— 先缩后放,
    // 在 125%/150% 缩放的屏上整屏发糊。
    if (deviceScale <= 0.0f)
        deviceScale = 1.0f;

    const int n = JZScreen::count();
    if (n <= 0)
        return JZImage();

    vector<JZRect> geos;
    vector<JZImage> imgs;
    int left = 0, top = 0, right = 0, bottom = 0;
    bool first = true;

    for (int i = 0; i < n; i++)
    {
        JZScreen s(i);
        if (!s.isValid())
            continue;
        JZRect g = s.geometry();
        if (g.width() <= 0 || g.height() <= 0)
            continue;
        JZImage img = s.grabScreen();
        if (img.isNull())
            continue;

        if (first)
        {
            left = g.x();
            top = g.y();
            right = g.x() + g.width();
            bottom = g.y() + g.height();
            first = false;
        }
        else
        {
            left = min(left, g.x());
            top = min(top, g.y());
            right = max(right, g.x() + g.width());
            bottom = max(bottom, g.y() + g.height());
        }
        geos.push_back(g);
        imgs.push_back(img);
    }

    if (geos.empty())
        return JZImage();

    const int lw = right - left;               // 逻辑桌面尺寸 (窗口定位用)
    const int lh = bottom - top;
    const int W = (int)lround(lw * deviceScale);   // 画布 = 物理像素
    const int H = (int)lround(lh * deviceScale);
    if (lw <= 0 || lh <= 0 || W <= 0 || H <= 0)
        return JZImage();

    vector<uint8_t> buf((size_t)W * H * 4, 0);
    for (size_t i = 0; i < geos.size(); i++)
    {
        const JZRect& g = geos[i];
        JZImage img = imgs[i];
        // 物理抓图 -> "该屏逻辑尺寸 * deviceScale"; 与目标一致时 (单屏/同缩放)
        // 不做任何缩放, 直接原样拷贝
        const int tw = (int)lround(g.width() * deviceScale);
        const int th = (int)lround(g.height() * deviceScale);
        if (tw <= 0 || th <= 0)
            continue;
        if (img.width() != tw || img.height() != th)
            img = img.scaled(tw, th, JZImage::AspectRatioMode::IgnoreAspectRatio,
                             JZImage::TransformationMode::SmoothTransformation);

        shared_ptr<JZImageData> d = img.data();
        if (!d || !d->pixels())
            continue;

        const uint8_t* src = d->pixels();
        const int sw = d->width();
        const int sh = d->height();
        const int dx = (int)lround((g.x() - left) * deviceScale);
        const int dy = (int)lround((g.y() - top) * deviceScale);

        for (int row = 0; row < sh; row++)
        {
            const int ty = dy + row;
            if (ty < 0 || ty >= H)
                continue;
            const uint8_t* srow = src + (size_t)row * sw * 4;
            int cx = dx, cw = sw;
            if (cx < 0)
            {
                srow += (size_t)(-cx) * 4;
                cw += cx;
                cx = 0;
            }
            if (cx + cw > W)
                cw = W - cx;
            if (cw <= 0)
                continue;
            memcpy(&buf[((size_t)ty * W + cx) * 4], srow, (size_t)cw * 4);
        }
    }

    if (logicalRect)
        *logicalRect = JZRect(left, top, lw, lh);   // 逻辑矩形 (窗口按它定位)
    return JZImage::fromRGBA(buf.data(), W, H);     // 图像本身是物理像素
}

JZImage JZScreenShot::fitTo(const JZImage& img, int maxW, int maxH)
{
    if (img.isNull() || maxW <= 0 || maxH <= 0)
        return img;

    const int w = img.width();
    const int h = img.height();
    if (w <= maxW && h <= maxH)
        return img;                        // 已经放得下, 不放大

    const float scale = min((float)maxW / w, (float)maxH / h);
    const int tw = max(1, (int)lround(w * scale));
    const int th = max(1, (int)lround(h * scale));

    JZImage out = img.scaled(tw, th);
    return out.isNull() ? img : out;
}

JZImage JZScreenShot::fit(const JZImage& img, int maxDim)
{
    return fitTo(img, maxDim, maxDim);
}

JZByteArray JZScreenShot::toPng(const JZImage& img)
{
    if (img.isNull())
        return JZByteArray();
    return img.encode(JZImage::Format::Png);
}

std::string JZScreenShot::toDataUrl(const JZImage& img, int maxDim)
{
    JZByteArray png = toPng(fit(img, maxDim));
    if (png.size() <= 0)
        return string();

    JZByteArray b64 = png.toBase64();
    string url = "data:image/png;base64,";
    url.append(b64.data(), (size_t)b64.size());
    return url;
}

JZImage JZScreenShot::decodeDataUrl(const std::string& dataUrl)
{
    string b64 = dataUrl;
    if (b64.compare(0, 5, "data:") == 0)
    {
        const size_t comma = b64.find(',');
        if (comma == string::npos)
            return JZImage();
        b64 = b64.substr(comma + 1);
    }
    if (b64.empty())
        return JZImage();

    JZByteArray raw = JZByteArray::fromBase64(JZByteArray(b64.data(), (int)b64.size()));
    if (raw.size() <= 0)
        return JZImage();
    return JZImage::decode(raw);
}

// 默认文件名: 本地时间 shot_yyyyMMdd_HHmmss
static string screenshotDefaultName()
{
    time_t now = time(nullptr);
    tm t;
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    char buf[40] = { 0 };
    strftime(buf, sizeof(buf), "shot_%Y%m%d_%H%M%S", &t);
    return string(buf);
}

std::string JZScreenShot::save(const JZImage& img, const std::string& dir,
                               const std::string& name)
{
    if (img.isNull() || dir.empty())
        return string();

    JZByteArray png = toPng(img);
    if (png.size() <= 0)
        return string();

    JZDir d(dir);
    if (!d.exists() && !d.mkpath(dir))
        return string();

    string full = dir;
    if (full.back() != '/' && full.back() != '\\')
        full += "/";
    full += (name.empty() ? screenshotDefaultName() : name) + ".png";

    JZFile f;
    if (!f.open(full, "wb"))
        return string();

    const int written = f.write(png.data(), png.size());
    f.close();
    return (written == png.size()) ? full : string();
}

// =====================================================================================
// 全屏标注截图窗口
// =====================================================================================

namespace
{

// 标注工具 (与画布按钮顺序一致)
enum ShotTool { ShotRect = 0, ShotEllipse = 1, ShotPen = 2 };

const char* const kToolNames[3] = { "矩形", "椭圆", "画笔" };
const JZColor kMarkColor(0xE8, 0x3B, 0x3B, 255);   // 标注红
const int kMarkWidth = 4;                          // 标注线宽(画布逻辑像素; 烧进物理像素图时按缩放放大)
const int kBtnW = 76;
const int kBtnH = 30;
const int kBtnGap = 8;

// 一笔标注: 矩形/椭圆记首尾两点, 画笔记整条折线
struct ShotMark
{
    int tool = ShotRect;
    vector<JZPoint> pts;
};

// 把一笔标注展开成折线: 矩形=闭合 4 边, 椭圆=内接于首尾矩形的采样点, 画笔=原折线。
void shotMarkPolyline(const ShotMark& mk, vector<JZPoint>& out)
{
    out.clear();
    if (mk.pts.empty())
        return;
    if (mk.tool == ShotPen)
    {
        out = mk.pts;
        return;
    }

    const JZPoint a = mk.pts.front();
    const JZPoint b = mk.pts.back();

    if (mk.tool == ShotRect)
    {
        out.push_back(a);
        out.push_back(JZPoint(b.x(), a.y()));
        out.push_back(b);
        out.push_back(JZPoint(a.x(), b.y()));
        out.push_back(a);
        return;
    }

    // 椭圆: 内接于 (a, b) 决定的矩形, 按周长采样(约每像素一段)
    const int cx = (a.x() + b.x()) / 2;
    const int cy = (a.y() + b.y()) / 2;
    const int rx = abs(b.x() - a.x()) / 2;
    const int ry = abs(b.y() - a.y()) / 2;
    if (rx <= 0 || ry <= 0)
        return;

    int segs = (int)lround(6.2832f * (rx + ry) / 2.0f);
    segs = max(24, min(segs, 4096));
    for (int i = 0; i <= segs; i++)
    {
        const float t = (float)i / (float)segs * 6.2831853f;
        out.push_back(JZPoint(cx + (int)lround(rx * cosf(t)),
                              cy + (int)lround(ry * sinf(t))));
    }
}

// ---- 把标注烧进 RGBA 像素(确认时调用一次) ----

inline void shotBlendPixel(vector<uint8_t>& buf, int w, int h, int x, int y,
                           const JZColor& c, float a)
{
    if (a <= 0.0f || x < 0 || y < 0 || x >= w || y >= h)
        return;
    if (a > 1.0f)
        a = 1.0f;
    uint8_t* p = &buf[((size_t)y * w + x) * 4];
    p[0] = (uint8_t)lround(p[0] + ((int)c.red() - p[0]) * a);
    p[1] = (uint8_t)lround(p[1] + ((int)c.green() - p[1]) * a);
    p[2] = (uint8_t)lround(p[2] + ((int)c.blue() - p[2]) * a);
    p[3] = 255;
}

// 画一个圆头"笔尖": 抗锯齿按到圆心距离做覆盖率
void shotStampDisc(vector<uint8_t>& buf, int w, int h, float cx, float cy,
                   float r, const JZColor& c)
{
    const int x0 = (int)floorf(cx - r - 1.0f), x1 = (int)ceilf(cx + r + 1.0f);
    const int y0 = (int)floorf(cy - r - 1.0f), y1 = (int)ceilf(cy + r + 1.0f);
    for (int y = y0; y <= y1; y++)
    {
        for (int x = x0; x <= x1; x++)
        {
            const float dx = (float)x + 0.5f - cx;
            const float dy = (float)y + 0.5f - cy;
            shotBlendPixel(buf, w, h, x, y, c,
                           r + 0.5f - sqrtf(dx * dx + dy * dy));
        }
    }
}

// 粗线段 = 沿线段等距盖笔尖(比按包围盒逐像素算距离快, 长斜线也不会退化成全屏扫描)
void shotStrokeSeg(vector<uint8_t>& buf, int w, int h, float x0, float y0,
                   float x1, float y1, float r, const JZColor& c)
{
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = sqrtf(dx * dx + dy * dy);
    const float step = max(0.5f, r * 0.5f);
    int n = (int)ceilf(len / step);
    if (n < 1)
        n = 1;
    for (int i = 0; i <= n; i++)
    {
        const float t = (float)i / (float)n;
        shotStampDisc(buf, w, h, x0 + dx * t, y0 + dy * t, r, c);
    }
}

void shotBurnPolyline(vector<uint8_t>& buf, int w, int h, const vector<JZPoint>& poly,
                      float r, const JZColor& c)
{
    if (poly.empty())
        return;
    if (poly.size() == 1)
    {
        shotStampDisc(buf, w, h, poly[0].x() + 0.5f, poly[0].y() + 0.5f, r, c);
        return;
    }
    for (size_t i = 1; i < poly.size(); i++)
        shotStrokeSeg(buf, w, h, poly[i - 1].x() + 0.5f, poly[i - 1].y() + 0.5f,
                      poly[i].x() + 0.5f, poly[i].y() + 0.5f, r, c);
}

// 原图 + 全部标注 -> 合成图(无标注时原样返回)
// 把标注烧进截图。scale = 图像像素 / 画布逻辑像素 (即窗口设备缩放): 标注坐标是
// 画布逻辑坐标, 而图像是物理像素, 烧制前必须按这个比例放大 (线宽同理), 否则标注
// 会偏到左上角一小块。
JZImage shotBurnMarks(const JZImage& src, const vector<ShotMark>& marks, float scale)
{
    if (src.isNull() || marks.empty())
        return src;

    shared_ptr<JZImageData> d = src.data();
    if (!d || !d->pixels())
        return src;

    const int w = d->width(), h = d->height();
    vector<uint8_t> buf((size_t)w * h * 4);
    memcpy(buf.data(), d->pixels(), buf.size());

    if (scale <= 0.0f)
        scale = 1.0f;

    vector<JZPoint> poly;
    for (size_t i = 0; i < marks.size(); i++)
    {
        shotMarkPolyline(marks[i], poly);
        if (scale != 1.0f)
            for (size_t k = 0; k < poly.size(); k++)
                poly[k] = JZPoint((int)lround(poly[k].x() * scale),
                                  (int)lround(poly[k].y() * scale));
        shotBurnPolyline(buf, w, h, poly, (kMarkWidth * scale) / 2.0f, kMarkColor);
    }
    return JZImage::fromRGBA(buf.data(), w, h);
}

} // namespace

// ==================== 标注画布 ====================
// 全屏窗口里的内容控件: 铺满整屏的截图 + 标注绘制 + 鼠标/键盘交互。
class JZShotCanvas : public JZWidget
{
    JZ_OBJECT_DECLARE(JZShotCanvas)

public:
    explicit JZShotCanvas(JZWidget* parent = nullptr);

    // 背景截图: 转成 JZPixmap 后每帧贴图只是一次位图拷贝 (转换只在 setShot 时
    // 做一次 —— 标注窗每帧重绘整窗, 4K 图逐帧现转会卡死)
    void setShot(const JZImage& img)
    {
        m_shot = img;
        m_shotPixmap = JZPixmap::fromImage(img);
        update();
    }
    void setTool(int tool);
    int  tool() const { return m_tool; }
    void undo();
    void layoutButtons();                 // 底部工具条(居中)排布

    const JZImage& shotImage() const { return m_shot; }
    const vector<ShotMark>& marks() const { return m_marks; }

jz_signals:
    JZSigVoid sigAccept;
    JZSigVoid sigReject;

protected:
    void paintEvent(JZPainterEvent* event) override;
    void resizeEvent(JZResizeEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseMoveEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;
    void keyPressEvent(JZKeyEvent* event) override;

private:
    void refreshTools();                  // 当前工具按钮加"●"标记
    void drawMark(JZPainter& p, const ShotMark& mk, vector<JZPoint>& poly);

    vector<ShotMark> m_marks;             // 已确认的标注
    JZImage m_shot;                       // 背景截图(尺寸 == 画布尺寸), 确认时烧标注用
    JZPixmap m_shotPixmap;                // m_shot 的显示用位图 (每帧贴图用这个)

    int  m_tool = ShotRect;
    bool m_drawing = false;
    ShotMark m_current;                   // 正在拖的一笔

    vector<JZPushButton*> m_tools;        // 矩形 / 椭圆 / 画笔
    vector<JZPushButton*> m_actions;      // 撤销 / 取消 / 确定
};
JZ_OBJECT_IMPL(JZShotCanvas, JZWidget)

JZShotCanvas::JZShotCanvas(JZWidget* parent)
    : JZWidget(parent)
    , sigAccept(this)
    , sigReject(this)
{
    setCursor(JZCursorShape::Cross);

    for (int i = 0; i < 3; i++)
    {
        JZPushButton* b = new JZPushButton(this, kBtnW, kBtnH, kToolNames[i]);
        connect(&b->sigClicked, this, [this, i]() { setTool(i); });
        m_tools.push_back(b);
    }

    JZPushButton* undoBtn = new JZPushButton(this, kBtnW, kBtnH, "撤销");
    JZPushButton* cancelBtn = new JZPushButton(this, kBtnW, kBtnH, "取消");
    JZPushButton* okBtn = new JZPushButton(this, kBtnW, kBtnH, "确定");
    connect(&undoBtn->sigClicked, this, [this]() { undo(); });
    connect(&cancelBtn->sigClicked, this, [this]() { sigReject.emit(); });
    connect(&okBtn->sigClicked, this, [this]() { sigAccept.emit(); });
    m_actions.push_back(undoBtn);
    m_actions.push_back(cancelBtn);
    m_actions.push_back(okBtn);

    refreshTools();
}

void JZShotCanvas::refreshTools()
{
    for (size_t i = 0; i < m_tools.size(); i++)
        m_tools[i]->setText(((int)i == m_tool ? "● " : "") + string(kToolNames[i]));
}

void JZShotCanvas::setTool(int tool)
{
    if (tool < ShotRect || tool > ShotPen)
        return;
    m_tool = tool;
    refreshTools();
}

void JZShotCanvas::undo()
{
    if (m_marks.empty())
        return;
    m_marks.pop_back();
    update();
}

void JZShotCanvas::layoutButtons()
{
    vector<JZPushButton*> all = m_tools;
    all.insert(all.end(), m_actions.begin(), m_actions.end());
    if (all.empty())
        return;

    const int total = (int)all.size() * kBtnW + ((int)all.size() - 1) * kBtnGap;
    int x = (width() - total) / 2;
    int y = height() - kBtnH - 14;
    if (x < 8)
        x = 8;
    if (y < 8)
        y = 8;

    for (size_t i = 0; i < all.size(); i++)
    {
        all[i]->setGeometry(x, y, kBtnW, kBtnH);
        x += kBtnW + kBtnGap;
    }
}

void JZShotCanvas::drawMark(JZPainter& p, const ShotMark& mk, vector<JZPoint>& poly)
{
    shotMarkPolyline(mk, poly);
    for (size_t i = 1; i < poly.size(); i++)
        p.drawLine(poly[i - 1].x(), poly[i - 1].y(), poly[i].x(), poly[i].y());
    if (poly.size() == 1)                 // 单点(点一下没拖动): 画一小段示意
        p.drawLine(poly[0].x(), poly[0].y(), poly[0].x() + 1, poly[0].y());
}

void JZShotCanvas::paintEvent(JZPainterEvent*)
{
    JZPainter p(this);
    const int w = width(), h = height();

    // 兜底底色只在截图盖不满画布时才需要: 截图与画布等大时这层 ~10ms 的
    // 全画布填充会被立刻完全覆盖 (4K 全屏标注每帧重绘整窗, 别白画)
    if (m_shot.isNull() || m_shot.width() < w || m_shot.height() < h)
        p.fillRect(0, 0, w, h, JZColor(0x1E, 0x1E, 0x1E));
    if (!m_shotPixmap.isNull())
        p.drawPixmap(m_shotPixmap, 0, 0, w, h);

    // 标注: 已确认的 + 正在拖的这一笔(同色同宽, 所见即所得)
    p.setPen(JZPen(kMarkColor, kMarkWidth));
    vector<JZPoint> poly;
    for (size_t i = 0; i < m_marks.size(); i++)
        drawMark(p, m_marks[i], poly);
    if (m_drawing)
        drawMark(p, m_current, poly);

    // 顶部操作提示
    const char* hint = "拖动绘制 · 1矩形 2椭圆 3画笔 · Shift正方形/正圆 · Ctrl+Z 撤销 · Enter/确定 完成 · Esc/取消 放弃";
    JZFont hf(12);
    const int tw = JZFontMetrics::textWidth(hint, hf);
    p.fillRect(12, 12, tw + 20, 26, JZColor(0x26, 0x26, 0x26));
    p.setFont(hf);
    p.setPen(JZPen(JZColor(0xF2, 0xF2, 0xF2), 1));
    p.drawText(22, 12 + JZFontMetrics::centeredTextTop(hf, 26), hint);
}

void JZShotCanvas::resizeEvent(JZResizeEvent* event)
{
    JZWidget::resizeEvent(event);
    layoutButtons();
}

void JZShotCanvas::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() == JZMouseEvent::LeftButton)
    {
        grabMouse();                      // 拖动期间独占鼠标(经过工具按钮也不中断)
        m_drawing = true;
        m_current.tool = m_tool;
        m_current.pts.clear();
        m_current.pts.push_back(event->localPos());
        update();
    }
    JZWidget::mousePressEvent(event);
}

void JZShotCanvas::mouseMoveEvent(JZMouseEvent* event)
{
    if (!m_drawing)
    {
        JZWidget::mouseMoveEvent(event);
        return;
    }

    const JZPoint pt = event->localPos();
    if (m_current.tool == ShotPen)
    {
        // 画笔: 逐点追加(重合点不重复记), 并给轨迹长度设上限防止极端长按爆内存
        const JZPoint& last = m_current.pts.back();
        if ((last.x() != pt.x() || last.y() != pt.y()) && m_current.pts.size() < 20000)
            m_current.pts.push_back(pt);
    }
    else
    {
        JZPoint end = pt;
        // Shift: 约束成正方形 / 正圆
        if ((event->modifiers() & JZKeyEvent::ShiftModifier) != 0)
        {
            const JZPoint a = m_current.pts.front();
            const int dx = end.x() - a.x();
            const int dy = end.y() - a.y();
            const int d = max(abs(dx), abs(dy));
            end = JZPoint(a.x() + (dx < 0 ? -d : d), a.y() + (dy < 0 ? -d : d));
        }
        if (m_current.pts.size() < 2)
            m_current.pts.resize(2);
        m_current.pts[1] = end;
    }
    update();
}

void JZShotCanvas::mouseReleaseEvent(JZMouseEvent* event)
{
    if (m_drawing && event->button() == JZMouseEvent::LeftButton)
    {
        m_drawing = false;
        releaseMouse();

        // 矩形/椭圆"点一下没拖动"不算一笔(误点), 画笔单击留下一个点是有意的
        bool valid = !m_current.pts.empty();
        if (valid && m_current.tool != ShotPen)
        {
            valid = (m_current.pts.size() >= 2 &&
                     (abs(m_current.pts[1].x() - m_current.pts[0].x()) >= 3 ||
                      abs(m_current.pts[1].y() - m_current.pts[0].y()) >= 3));
        }
        if (valid)
            m_marks.push_back(m_current);

        m_current.pts.clear();
        update();
    }
    JZWidget::mouseReleaseEvent(event);
}

void JZShotCanvas::keyPressEvent(JZKeyEvent* event)
{
    const uint32_t k = event->key();
    if (k == '1') { setTool(ShotRect); event->accept(); return; }
    if (k == '2') { setTool(ShotEllipse); event->accept(); return; }
    if (k == '3') { setTool(ShotPen); event->accept(); return; }
    if ((k == 'z' || k == 'Z') && event->ctrl()) { undo(); event->accept(); return; }
    if (k == JZKey::Key_Return) { sigAccept.emit(); event->accept(); return; }
    // Esc 不在这里吞掉: 冒泡到窗口 -> 取消(见 JZShotWindow::keyPressEvent)
    JZWidget::keyPressEvent(event);
}

// ==================== 全屏截图窗口 ====================
// 无边框(Popup)顶层窗口, 覆盖整个虚拟桌面; 内容只有一块铺满全屏的标注画布。
class JZShotWindow : public JZWindow
{
    JZ_OBJECT_DECLARE(JZShotWindow)

public:
    JZShotWindow(const JZImage& shot, const JZRect& rc, JZWidget* owner)
        : JZWindow(owner)
    {
        setWindowType(Popup);          // 无边框 + 不进任务栏
        setTitle("JZScreenShot");
        moveScreen(rc.x(), rc.y());    // 与虚拟桌面完全重合
        resize(rc.width(), rc.height());

        m_canvas = new JZShotCanvas(this);
        m_canvas->setShot(shot);
        m_canvas->setGeometry(0, 0, rc.width(), rc.height());
        m_canvas->layoutButtons();

        connect(&m_canvas->sigAccept, this, [this]() { finish(true); });
        connect(&m_canvas->sigReject, this, [this]() { finish(false); });
    }

    // 弹出并阻塞到用户完成/取消; 返回 true = 已确认
    bool exec()
    {
        m_accepted = false;
        show();
        m_canvas->setFocus();          // 键盘(Enter/Esc/1·2·3/Ctrl+Z)直达画布
        execModal(true);
        return m_accepted;
    }

    // 截图 + 标注 的合成结果
    JZImage resultImage() const
    {
        // 画布坐标是逻辑像素, 截图是物理像素: 烧标注时按两者实际比例放大
        // (用尺寸比值而不是 device()->scale(), 免依赖设备状态, 且与拼图口径一致)
        const JZImage& shot = m_canvas->shotImage();
        const int cw = m_canvas->width();
        const float s = (cw > 0 && shot.width() > 0)
                            ? (float)shot.width() / (float)cw
                            : 1.0f;
        return shotBurnMarks(shot, m_canvas->marks(), s);
    }

protected:
    // Esc = 取消(覆盖 Popup 默认的"隐藏但不表态")
    void keyPressEvent(JZKeyEvent* event) override
    {
        if (event->key() == JZKey::Key_Escape)
        {
            finish(false);
            event->accept();
            return;
        }
        JZWindow::keyPressEvent(event);
    }

    // 原生关闭(X / Alt+F4): 只隐藏不销毁, 等价取消
    void closeEvent(JZCloseEvent* event) override
    {
        event->ignore();
        finish(false);
    }

private:
    void finish(bool accepted)
    {
        m_accepted = accepted;
        hide();                        // 隐藏即退出 execModal
    }

    JZShotCanvas* m_canvas = nullptr;
    bool m_accepted = false;
};
JZ_OBJECT_IMPL(JZShotWindow, JZWindow)

JZImage JZScreenShot::captureInteractive(JZWidget* owner)
{
    // 标注窗口的设备缩放取自 owner 所在显示器 (JZWindow::createWindow 就是这么定的),
    // 抓屏按同一缩放拼图 -> 窗口铺满桌面时贴图恰好 1:1, 不发糊。
    float scale = 1.0f;
    if (owner)
    {
        if (JZWindow* w = owner->window())
            scale = JZScreen::logicalDpiFor(w->nativeHandle()) / 96.0f;
    }
    else
    {
        scale = JZScreen::primaryScreen().devicePixelRatio();
    }
    if (scale <= 0.0f)
        scale = 1.0f;

    JZRect rc;
    JZImage shot = captureVirtualDesktop(&rc, scale);
    if (shot.isNull() || rc.width() <= 0 || rc.height() <= 0)
        return JZImage();

    JZShotWindow win(shot, rc, owner);
    if (!win.exec())
        return JZImage();
    return win.resultImage();
}
