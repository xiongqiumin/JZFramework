#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include "JZPainter.h"
#include "JZWidget.h"
#include "JZWindow.h"
#include "JZDisplayDevice.h"

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

JZPainter::~JZPainter()
{
    // 平衡本 painter 发出的全部 Push (构造/状态 setter/save), 保证绘制流严格配对。
    while (m_streamDepth > 0) {
        auto t = std::make_shared<JZDrawPopTask>();
        m_layer->addTask(t);
        --m_streamDepth;
    }
}

void JZPainter::pushState()
{
    // Push 语义: 保存绘制流当前状态, 并应用携带的新状态 (pen/brush/font/clip)。
    auto t = std::make_shared<JZDrawPushTask>();
    t->pen = m_pen;
    t->brush = m_brush;
    t->font = m_font;
    t->clip = m_clip;
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
    m_stack.push_back({m_pen, m_brush, m_font});
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
    auto t = std::make_shared<JZDrawPopTask>();
    m_layer->addTask(t);
    --m_streamDepth;
}

JZRect JZPainter::applyAncestorClips(JZWidget* widget, const JZRect& globalClip) const
{
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
    // 粗略剔除: 线段显示坐标包围盒完全出裁剪区则不提交。
    int lx = std::min(x1, x2), ly = std::min(y1, y2);
    int lw = std::abs(x2 - x1) + 1, lh = std::abs(y2 - y1) + 1;
    if (isCulled(JZRect(m_ox + lx, m_oy + ly, lw, lh)))
        return;

    auto t = std::make_shared<JZLineTask>();
    t->x1 = m_ox + x1;
    t->y1 = m_oy + y1;
    t->x2 = m_ox + x2;
    t->y2 = m_oy + y2;
    m_layer->addTask(t);
}

void JZPainter::drawRect(int x, int y, int w, int h)
{
    JZRect area(m_ox + x, m_oy + y, w, h);
    if (isCulled(area))
        return;

    auto t = std::make_shared<JZBorderTask>();
    t->area = area;
    m_layer->addTask(t);
}

void JZPainter::drawRoundRect(int x, int y, int w, int h, int radius)
{
    JZRect area(m_ox + x, m_oy + y, w, h);
    if (isCulled(area))
        return;

    // 圆角矩形 = 圆角填充 + 圆角描边 (填充色为 brush 的调用参数化快照)
    auto fill = std::make_shared<JZFillRectTask>();
    fill->area = area;
    fill->color = m_brush.color();
    fill->radius = radius;
    m_layer->addTask(fill);

    auto border = std::make_shared<JZBorderTask>();
    border->area = area;
    border->radius = radius;
    m_layer->addTask(border);
}

void JZPainter::fillRect(const JZRect &rc, const JZColor& color)
{
    fillRect(rc.x(), rc.y(), rc.width(), rc.height(), color);
}

void JZPainter::fillRect(int x, int y, int w, int h, const JZColor& color)
{
    JZRect area(m_ox + x, m_oy + y, w, h);
    if (isCulled(area))
        return;

    auto t = std::make_shared<JZFillRectTask>();
    t->area = area;
    t->color = color;
    m_layer->addTask(t);
}

void JZPainter::fillRoundRect(int x, int y, int w, int h, int radius, const JZColor& color)
{
    JZRect area(m_ox + x, m_oy + y, w, h);
    if (isCulled(area))
        return;

    auto t = std::make_shared<JZFillRectTask>();
    t->area = area;
    t->color = color;
    t->radius = radius;
    m_layer->addTask(t);
}

void JZPainter::drawArc(int x, int y, int radius, int startAngle, int endAngle)
{
    // 粗略剔除: 以圆心+半径为外接方框。
    JZRect bound(m_ox + x - radius, m_oy + y - radius, 2 * radius + 1, 2 * radius + 1);
    if (isCulled(bound))
        return;

    // 角度内部换算为 1/10 度 (LVGL 单位): 传入的 startAngle/endAngle 为度, *10 得 1/10 度。
    // 例如 endAngle=360 -> 3600 (整圆), 与 JZArcTask 默认值一致。
    auto t = std::make_shared<JZArcTask>();
    t->arcCx = m_ox + x;
    t->arcCy = m_oy + y;
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

    // 粗略包围盒: 用字号估高、字符数估宽 (仅作出屏剔除用)。
    int th = m_font.size();
    int tw = (int)std::strlen(text) * m_font.size() / 2;
    if (isCulled(JZRect(m_ox + x, m_oy + y, tw, th)))
        return;

    auto t = std::make_shared<JZTextTask>();
    t->text = text;
    // area 作为文本绘制原点区域 (top-left 对齐)
    t->area = JZRect(m_ox + x, m_oy + y, 1, 1);
    m_layer->addTask(t);
}

void JZPainter::drawImage(const JZImage& img, int x, int y, int w, int h)
{
    JZRect area(m_ox + x, m_oy + y, w, h);
    if (isCulled(area))
        return;

    auto t = std::make_shared<JZImageTask>();
    t->image = img.data();
    t->area = area;
    m_layer->addTask(t);
}
