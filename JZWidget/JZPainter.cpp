#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include "JZPainter.h"
#include "JZWidget.h"
#include "JZWindow.h"
#include "JZDisplayDevice.h"
#include "JZGdiBackend.h"
#include "JZTextEmoji.h"
#include "JZEmojiImageMap.h"
#include "JZFontMetrics.h"

JZPainter::JZPainter(JZWidget* widget)
{
    // 契约: painter 只在 paintEvent 流程内构造, widget 必挂在有效窗口/设备上, 不做空保护。
    JZDisplayDevice* device = widget->window()->device();
    m_layer = device->layer();

    // 坐标转换: 绘制原点对齐到本 widget 在显示坐标下的左上角,
    // 这样 widget 内部用本地坐标 (0,0) 绘制即可, 由 m_ox/m_oy 平移到屏幕坐标。
    JZRect g = widget->mapTo(widget->window(), widget->rect());
    m_ox = g.left();
    m_oy = g.top();
    m_screen = widget->window()->rect();
    m_widget = widget;

    // 裁剪 = 本轮脏区 ∩ 控件全局 rect (border-box 外框)。
    // 控件在 paintEvent 里自建 JZPainter, clip 的权威设定点在构造函数:
    //   - 内容层不可能画出控件 rect (长文本/粗画笔不再越界盖到邻居);
    //   - 部分失效时, 根窗口的整窗白填也被限制在脏区内。
    JZRect dirty = device->dirtyRect();
    m_clip = dirty.isEmpty() ? g : dirty.intersect(g);

    // 祖先裁剪 (Qt WA_ClipChildren 语义): 向上找到开启 setClipChildren 的祖先控件
    // (如滚动区 viewport), 把裁剪区收缩到其显示范围, 越界内容 (被移出视口的内容
    // 子控件) 因此不会画到视口外。
    m_clip = applyAncestorClips(widget, m_clip);

    // 进入 JZPainter 时发 Push, 把起始状态 (pen/brush/font/clip) 写入绘制流。
    pushState();
}

JZPainter::JZPainter(JZDisplayDevice* device, const JZRect& canvas)
{
    // 离屏**设备**模式: 任务写进设备的图层, 析构时由设备一次性回放到它自己的帧缓冲。
    // 与"作用于 JZImage"那条路的区别: 目标是后端原生缓冲 (GDI+ 的 DIB), 不必每帧
    // 把整幅画布在 RGBA <-> PARGB 之间来回拷 (见 JZGdiBackend::renderLayerToImage)。
    // 坐标与窗口那条路同构: 逻辑坐标 + 设备 DPI 缩放, 放置偏移为 0。
    m_layer = (device ? device->layer() : &m_ownLayer);
    m_deviceTarget = device;
    m_widget = nullptr;
    m_ox = 0;
    m_oy = 0;
    m_screen = canvas;
    m_clip = canvas;
    pushState();
}

JZPainter::JZPainter(JZImage& image, float scale)
{
    // 离屏模式: painter 不再向窗口图层追加任务, 而是自持一个图层, 坐标系对齐到
    // image 像素 (原点 0,0, 无父链/祖先裁剪)。析构时把图层合成回 image 像素缓冲。
    m_layer = &m_ownLayer;
    m_imageTarget = &image;
    m_widget = nullptr;
    // 逻辑 -> image 像素的缩放: 尺寸校验与裁剪仍按 image 自己的像素 (物理) 来,
    // 各 drawXxx 用逻辑坐标提交, 由它统一换算 (等价窗口那条路的 DPI 缩放)。
    m_scale = (scale > 0.0f) ? scale : 1.0f;

    int w = image.width();
    int h = image.height();
    m_ox = 0;
    m_oy = 0;
    m_screen = JZRect(0, 0, w, h);
    m_clip = m_screen;

    // 进入 JZPainter 时发 Push, 把起始状态 (pen/brush/font/clip) 写入绘制流。
    pushState();
}

JZPainter::~JZPainter()
{
    // 平衡本 painter 发出的全部 Push (构造/状态 setter/save), 保证绘制流严格配对。
    while (m_streamDepth > 0) {
        auto t = std::make_shared<JZDrawPopTask>();
        m_layer->addTask(t);
        --m_streamDepth;
    }

    endDevice();
}

void JZPainter::endDevice()
{
    // 离屏模式: 把本 painter 的图层合成回目标 image 的像素缓冲。
    if (m_imageTarget) {
        JZGdiBackend::ensureGdiplus();
        JZGdiBackend::renderLayerToImage(*m_imageTarget, m_layer, m_scale);
        JZGdiBackend::releaseGdiplus();
        m_imageTarget = nullptr;
    }
    // 离屏设备模式: 把本 painter 的任务回放到设备自己的帧缓冲 (原生布局, 零转换)。
    if (m_deviceTarget) {
        m_deviceTarget->renderLayer(m_layer);
        m_deviceTarget = nullptr;
    }
}

void JZPainter::pushState()
{
    // Push 语义: 保存绘制流当前状态, 并应用携带的新状态 (pen/brush/font/clip/matrix)。
    auto t = std::make_shared<JZDrawPushTask>();
    t->pen = m_pen;
    t->brush = m_brush;
    t->font = m_font;
    t->clip = m_clip;
    t->matrix = currentTaskMatrix();
    t->ox = m_ox;
    t->oy = m_oy;
    m_layer->addTask(t);
    ++m_streamDepth;
}

void JZPainter::setPen(const JZPen& pen)
{
    m_pen = pen;
    auto t = std::make_shared<JZDrawChangeTask>();
    t->pen = JZOptional<JZPen>::Some(pen);
    m_layer->addTask(t);
}

JZPen JZPainter::pen() const
{
    return m_pen;
}

void JZPainter::setBrush(const JZBrush& brush)
{
    m_brush = brush;
    auto t = std::make_shared<JZDrawChangeTask>();
    t->brush = JZOptional<JZBrush>::Some(brush);
    m_layer->addTask(t);
}

JZBrush JZPainter::brush() const
{
    return m_brush;
}

void JZPainter::setFont(const JZFont& font)
{
    m_font = font;
    auto t = std::make_shared<JZDrawChangeTask>();
    t->font = JZOptional<JZFont>::Some(font);
    m_layer->addTask(t);
}

JZFont JZPainter::font() const
{
    return m_font;
}

void JZPainter::setClip(const JZRect& clip)
{
    // 裁剪在绘制流里一律以显示(窗口)坐标表示: isCulled 拿它减去图元的显示包围盒,
    // 后端 replayTasks 直接 g.SetClip(它 * scale)。setClip 传的是控件本地坐标
    // (与 drawXxx 各接口一致), 必须先平移到显示坐标, 否则裁剪区会被放到窗口
    // 左上角, 控件内容整片消失。
    JZRect global(m_ox + clip.x(), m_oy + clip.y(),
                  clip.width(), clip.height());
    m_clip = applyAncestorClips(m_widget, global);
    auto t = std::make_shared<JZDrawChangeTask>();
    t->clip = JZOptional<JZRect>::Some(m_clip);
    m_layer->addTask(t);
}

void JZPainter::save()
{
    // save 语义: 绘制流栈 +1 (Push 携带当前状态, 状态内容不变)。
    m_stack.push_back({m_pen, m_brush, m_font, m_matrix});
    pushState();
}

void JZPainter::restore()
{
    if (m_stack.empty())
        return;
    State s = m_stack.back();
    m_stack.pop_back();
    m_pen = s.pen;
    m_brush = s.brush;
    m_font = s.font;
    m_matrix = s.matrix;
    auto t = std::make_shared<JZDrawPopTask>();
    m_layer->addTask(t);
    --m_streamDepth;
}

void JZPainter::setTransform(const JZMatrix2D& m)
{
    m_matrix = m;
    auto t = std::make_shared<JZDrawChangeTask>();
    t->matrix = JZOptional<JZMatrix2D>::Some(currentTaskMatrix());
    m_layer->addTask(t);
}

JZMatrix2D JZPainter::transform() const
{
    return m_matrix;
}

void JZPainter::resetTransform()
{
    m_matrix.reset();
    auto t = std::make_shared<JZDrawChangeTask>();
    t->matrix = JZOptional<JZMatrix2D>::Some(currentTaskMatrix());
    m_layer->addTask(t);
}

void JZPainter::translate(double dx, double dy)
{
    m_matrix.translate(dx, dy);
    auto t = std::make_shared<JZDrawChangeTask>();
    t->matrix = JZOptional<JZMatrix2D>::Some(currentTaskMatrix());
    m_layer->addTask(t);
}

void JZPainter::rotate(double deg)
{
    m_matrix.rotate(deg);
    auto t = std::make_shared<JZDrawChangeTask>();
    t->matrix = JZOptional<JZMatrix2D>::Some(currentTaskMatrix());
    m_layer->addTask(t);
}

void JZPainter::scale(double sx, double sy)
{
    m_matrix.scale(sx, sy);
    auto t = std::make_shared<JZDrawChangeTask>();
    t->matrix = JZOptional<JZMatrix2D>::Some(currentTaskMatrix());
    m_layer->addTask(t);
}

JZMatrix2D JZPainter::currentTaskMatrix() const
{
    // 仅控件自身的本地变换; 放置偏移 (m_ox/m_oy) 由 Push 单独携带, 不计入矩阵。
    return m_matrix;
}

JZRect JZPainter::applyAncestorClips(JZWidget* widget, const JZRect& globalClip) const
{
    // 离屏模式 (作用于 JZImage) 没有父链/祖先控件, 不做任何收窄。
    if (!widget)
        return globalClip;

    JZRect c = globalClip;
    JZWidget* root = widget->window();
    for (JZWidget* p = widget->parentWidget(); p && p != root; p = p->parentWidget()) {
        if (!p->clipChildren())
            continue;
        c = c.intersect(p->mapTo(root, p->rect()));
    }
    return c;
}

bool JZPainter::isCulled(const JZRect& displayBounds) const
{
    // 完全超出裁剪区(优先 m_clip, 否则屏幕)的图元不提交。
    JZRect c = m_clip.isEmpty() ? m_screen : m_clip;
    if (c.isEmpty())
        return false; // 无可用边界, 不剔除
    return !displayBounds.intersects(c);
}

void JZPainter::drawLine(int x1, int y1, int x2, int y2)
{
    // 粗略剔除: 线段显示坐标包围盒完全出裁剪区则不提交 (仅单位阵时做, 旋转后包围盒不准)。
    // 任务以本地坐标提交, 变换/偏移由后端经世界变换统一施加。
    if (m_matrix.isIdentity()) {
        int lx = std::min(x1, x2) + m_ox, ly = std::min(y1, y2) + m_oy;
        int lw = std::abs(x2 - x1) + 1, lh = std::abs(y2 - y1) + 1;
        if (isCulled(JZRect(lx, ly, lw, lh)))
            return;
    }

    auto t = std::make_shared<JZLineTask>();
    t->x1 = x1; t->y1 = y1;
    t->x2 = x2; t->y2 = y2;
    m_layer->addTask(t);
}

void JZPainter::drawRect(int x, int y, int w, int h)
{
    if (m_matrix.isIdentity()) {
        JZRect area(m_ox + x, m_oy + y, w, h);
        if (isCulled(area))
            return;
    }
    // 本地坐标提交; 旋转/缩放由后端世界变换统一施加 (描边内缩半笔宽在后端处理)。
    auto t = std::make_shared<JZBorderTask>();
    t->area = JZRect(x, y, w, h);
    m_layer->addTask(t);
}

void JZPainter::drawRoundRect(int x, int y, int w, int h, int radius)
{
    if (m_matrix.isIdentity()) {
        JZRect area(m_ox + x, m_oy + y, w, h);
        if (isCulled(area))
            return;
    }
    // 圆角矩形 = 圆角填充 + 圆角描边 (填充色为 brush 的调用参数化快照)。
    // 本地坐标提交; 非单位阵时圆角随世界变换缩放/旋转 (由后端处理)。
    auto fill = std::make_shared<JZFillRectTask>();
    fill->area = JZRect(x, y, w, h);
    fill->color = m_brush.color();
    fill->radius = radius;
    m_layer->addTask(fill);
    auto border = std::make_shared<JZBorderTask>();
    border->area = JZRect(x, y, w, h);
    border->radius = radius;
    m_layer->addTask(border);
}

void JZPainter::fillRect(const JZRect &rc, const JZColor& color)
{
    fillRect(rc.x(), rc.y(), rc.width(), rc.height(), color);
}

void JZPainter::fillRect(int x, int y, int w, int h, const JZColor& color)
{
    if (m_matrix.isIdentity()) {
        JZRect area(m_ox + x, m_oy + y, w, h);
        if (isCulled(area))
            return;
    }
    auto t = std::make_shared<JZFillRectTask>();
    t->area = JZRect(x, y, w, h);
    t->color = color;
    m_layer->addTask(t);
}

void JZPainter::fillRoundRect(int x, int y, int w, int h, int radius, const JZColor& color)
{
    if (m_matrix.isIdentity()) {
        JZRect area(m_ox + x, m_oy + y, w, h);
        if (isCulled(area))
            return;
    }
    auto t = std::make_shared<JZFillRectTask>();
    t->area = JZRect(x, y, w, h);
    t->color = color;
    t->radius = radius;
    m_layer->addTask(t);
}

void JZPainter::drawArc(int x, int y, int radius, int startAngle, int endAngle)
{
    // 粗略剔除: 以圆心+半径为外接方框 (仅单位阵时)。
    if (m_matrix.isIdentity()) {
        JZRect bound(m_ox + x - radius, m_oy + y - radius, 2 * radius + 1, 2 * radius + 1);
        if (isCulled(bound))
            return;
    }

    // 角度内部换算为 1/10 度 (LVGL 单位): 传入的 startAngle/endAngle 为度, *10 得 1/10 度。
    // 半径/圆心均为本地坐标, 缩放与旋转由后端世界变换统一施加。
    auto t = std::make_shared<JZArcTask>();
    t->arcCx = x;
    t->arcCy = y;
    t->arcRadius = radius;
    t->arcStartAngle = startAngle * 10;
    t->arcEndAngle = endAngle * 10;
    t->arcRounded = true;
    m_layer->addTask(t);
}

void JZPainter::drawText(int x, int y, const char* text)
{
    if (!text || text[0] == '\0')
        return;

    // emoji 感知分段: 文本段走文字任务, emoji 段命中图集则走位图任务,
    // 二者叠加即为完整一行(与 JZAbstractTextEdit / ChatView 同一口径)。
    const std::vector<JZTextEmojiRun> runs = jzSegmentTextEmoji(std::string(text), m_font);

    // 出屏剔除(仅单位阵时, 用分段累计宽度估计包围盒)
    if (m_matrix.isIdentity()) {
        int th = JZFontMetrics::textHeight(m_font);
        int tw = 0;
        for (const auto& r : runs)
            tw += r.width;
        if (isCulled(JZRect(m_ox + x, m_oy + y, tw, th)))
            return;
    }

    int cx = x;
    for (const auto& r : runs) {
        if (r.text.empty()) {
            cx += r.width;
            continue;
        }
        if (r.emoji) {
            // emoji 段: 命中内置图集则画位图(与文本段顶对齐), 否则降级为字体绘制。
            const int box = JZFontMetrics::textHeight(m_font);
            const JZPixmap* pm = JZEmojiImageMap::instance().findPixmap(r.text, box);
            if (pm && !pm->isNull()) {
                drawPixmap(*pm, cx, y, box, box);
                cx += r.width;
                continue;
            }
        }
        // 文本段 / 降级 emoji: 各自生成独立文字任务(变换由后端施加)。
        auto t = std::make_shared<JZTextTask>();
        t->text = r.text;
        t->area = JZRect(cx, y, 1, 1);   // 本地坐标, top-left 对齐
        m_layer->addTask(t);
        cx += r.width;
    }
}

void JZPainter::drawImage(const JZImage& img, int x, int y, int w, int h)
{
    if (m_matrix.isIdentity() && isCulled(JZRect(m_ox + x, m_oy + y, w, h)))
        return;

    auto t = std::make_shared<JZImageTask>();
    t->image = img.data();
    t->area = JZRect(x, y, w, h);   // 本地坐标, 缩放/旋转由后端世界变换施加
    m_layer->addTask(t);
}

void JZPainter::drawPixmap(const JZPixmap& pixmap, int x, int y, int w, int h)
{
    if (pixmap.isNull())
        return;
    if (m_matrix.isIdentity() && isCulled(JZRect(m_ox + x, m_oy + y, w, h)))
        return;

    auto t = std::make_shared<JZPixmapTask>();
    t->pixmap = pixmap.data();
    t->area = JZRect(x, y, w, h);   // 本地坐标, 缩放/旋转由后端世界变换施加
    m_layer->addTask(t);
}

// ==================== 矢量路径 / 曲线 / 椭圆 ====================

// 把路径 (本地坐标) 直接写入绘制流; stroke/fill 决定描边/填充。
// 变换/偏移由后端经世界变换统一施加, 不再在 painter 端烘焙进坐标。
void JZPainter::addPathTask(const JZPainterPath& path, bool stroke, bool fill,
                            const JZOptional<JZColor>& fillColor)
{
    auto t = std::make_shared<JZPathTask>();
    t->path = path;
    t->stroke = stroke;
    t->fill = fill;
    t->fillColor = fillColor;
    m_layer->addTask(t);
}

void JZPainter::drawPath(const JZPainterPath& path)
{
    addPathTask(path, true, false);
}

void JZPainter::fillPath(const JZPainterPath& path)
{
    addPathTask(path, false, true);
}

void JZPainter::drawEllipse(int x, int y, int w, int h)
{
    JZPainterPath p;
    p.addEllipse(x, y, w, h);
    drawPath(p);
}

void JZPainter::fillEllipse(int x, int y, int w, int h, const JZColor& color)
{
    JZPainterPath p;
    p.addEllipse(x, y, w, h);
    addPathTask(p, false, true, JZOptional<JZColor>::Some(color));
}

void JZPainter::drawCurve(const std::vector<JZPoint>& points)
{
    if (points.size() < 2)
        return;

    JZPainterPath p;
    p.moveTo(points[0]);
    int n = (int)points.size();
    // Catmull-Rom -> 三次贝塞尔: 经过每个控制点, 端点用邻近点做切线外推。
    for (int i = 0; i < n - 1; ++i) {
        double x0 = (i > 0 ? points[i - 1].x() : points[i].x());
        double y0 = (i > 0 ? points[i - 1].y() : points[i].y());
        double x1 = points[i].x(),     y1 = points[i].y();
        double x2 = points[i + 1].x(), y2 = points[i + 1].y();
        double x3 = (i + 2 < n ? points[i + 2].x() : points[i + 1].x());
        double y3 = (i + 2 < n ? points[i + 2].y() : points[i + 1].y());
        double c1x = x1 + (x2 - x0) / 6.0;
        double c1y = y1 + (y2 - y0) / 6.0;
        double c2x = x2 - (x3 - x1) / 6.0;
        double c2y = y2 - (y3 - y1) / 6.0;
        p.cubicTo(c1x, c1y, c2x, c2y, x2, y2);
    }
    drawPath(p);
}
