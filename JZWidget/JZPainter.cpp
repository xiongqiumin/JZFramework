#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include "JZPainter.h"
#include "JZWidget.h"
#include "JZDisplayDevice.h"

JZPainter::JZPainter(JZWidget* widget)
{
    JZDisplayDevice *device = widget->display();
    if (device) {
        m_layer = device->layer();
        // 屏幕可见区域 = 显示设备几何 (0,0,w,h), 用于完全出屏的粗略剔除。
        m_screen = device->geometry();
    }

    // 坐标转换: 绘制原点对齐到本 widget 在显示坐标下的左上角,
    // 这样 widget 内部用本地坐标 (0,0) 绘制即可, 由 m_ox/m_oy 平移到屏幕坐标。
    JZRect g = widget->toGlobal(widget->rect());
    m_ox = g.left();
    m_oy = g.top();

    // 进入 JZPainter 时发送一次 Changed, 记录绘制流起始的 pen/brush/font 状态快照。
    if (m_layer) {
        auto t = std::make_shared<JZChangedTask>();
        t->clip = m_clip;
        t->savedPen = m_pen;
        t->savedBrush = m_brush;
        t->savedFont = m_font;
        m_layer->addTask(t);
    }
}

void JZPainter::setPen(const JZPen& pen)
{
    m_pen = pen;
}

JZPen JZPainter::pen() const
{
    return m_pen;
}

void JZPainter::setBrush(const JZBrush& brush)
{
    m_brush = brush;
}

JZBrush JZPainter::brush() const
{
    return m_brush;
}

void JZPainter::setFont(const JZFont& font)
{
    m_font = font;
}

JZFont JZPainter::font() const
{
    return m_font;
}

void JZPainter::save()
{
    // 把当前状态压栈, 并往绘制流追加一个携带快照的 Changed 任务 (save 语义)。
    m_stack.push_back({m_pen, m_brush, m_font});
    if (m_layer) {
        auto t = std::make_shared<JZChangedTask>();
        t->clip = m_clip;
        t->savedPen = m_pen;
        t->savedBrush = m_brush;
        t->savedFont = m_font;
        m_layer->addTask(t);
    }
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
    if (m_layer) {
        auto t = std::make_shared<JZChangedTask>();
        t->clip = m_clip;
        t->savedPen = s.pen;
        t->savedBrush = s.brush;
        t->savedFont = s.font;
        m_layer->addTask(t);
    }
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
    if (!m_layer)
        return;

    // 粗略剔除: 线段显示坐标包围盒完全出裁剪区则不提交。
    int lx = std::min(x1, x2), ly = std::min(y1, y2);
    int lw = std::abs(x2 - x1) + 1, lh = std::abs(y2 - y1) + 1;
    if (isCulled(JZRect(m_ox + lx, m_oy + ly, lw, lh)))
        return;

    auto t = std::make_shared<JZLineTask>();
    t->clip = m_clip;
    t->pen = m_pen;
    t->x1 = m_ox + x1;
    t->y1 = m_oy + y1;
    t->x2 = m_ox + x2;
    t->y2 = m_oy + y2;
    m_layer->addTask(t);
}

void JZPainter::drawRect(int x, int y, int w, int h)
{
    if (!m_layer)
        return;

    JZRect area(m_ox + x, m_oy + y, w, h);
    if (isCulled(area))
        return;

    auto t = std::make_shared<JZBorderTask>();
    t->clip = m_clip;
    t->area = area;
    t->pen = m_pen;
    m_layer->addTask(t);
}

void JZPainter::drawRoundRect(int x, int y, int w, int h, int radius)
{
    if (!m_layer) return;
    JZRect area(m_ox + x, m_oy + y, w, h);
    if (isCulled(area))
        return;

    // 圆角矩形 = 圆角填充 + 圆角描边
    auto fill = std::make_shared<JZFillRectTask>();
    fill->clip = m_clip;
    fill->area = area;
    fill->color = m_brush.color();
    fill->radius = radius;
    m_layer->addTask(fill);

    auto border = std::make_shared<JZBorderTask>();
    border->clip = m_clip;
    border->area = area;
    border->pen = m_pen;
    border->radius = radius;
    m_layer->addTask(border);
}

void JZPainter::fillRect(int x, int y, int w, int h, const JZColor& color)
{
    if (!m_layer)
        return;

    JZRect area(m_ox + x, m_oy + y, w, h);
    if (isCulled(area))
        return;

    auto t = std::make_shared<JZFillRectTask>();
    t->clip = m_clip;
    t->area = area;
    t->color = color;
    m_layer->addTask(t);
}

void JZPainter::drawArc(int x, int y, int radius, int startAngle, int endAngle)
{
    if (!m_layer)
        return;

    // 粗略剔除: 以圆心+半径为外接方框。
    JZRect bound(m_ox + x - radius, m_oy + y - radius, 2 * radius + 1, 2 * radius + 1);
    if (isCulled(bound))
        return;

    // 角度内部换算为 1/10 度 (LVGL 单位): 传入的 startAngle/endAngle 为度, *10 得 1/10 度。
    // 例如 endAngle=360 -> 3600 (整圆), 与 JZArcTask 默认值一致。
    auto t = std::make_shared<JZArcTask>();
    t->clip = m_clip;
    t->pen = m_pen;
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
    if (!m_layer || !text || text[0] == '\0')
        return;

    // 粗略包围盒: 用字号估高、字符数估宽 (无字体度量, 仅作出屏剔除用)。
    int th = m_font.size();
    int tw = (int)std::strlen(text) * m_font.size() / 2;
    if (isCulled(JZRect(m_ox + x, m_oy + y, tw, th)))
        return;

    auto t = std::make_shared<JZTextTask>();
    t->clip = m_clip;
    t->color = m_pen.color();
    t->text = text;
    t->font = m_font;
    // area 作为文本绘制原点区域 (top-left 对齐)
    t->area = JZRect(m_ox + x, m_oy + y, 1, 1);
    m_layer->addTask(t);
}

void JZPainter::drawImage(const JZImage& img, int x, int y, int w, int h)
{
    if (!m_layer)
        return;

    JZRect area(m_ox + x, m_oy + y, w, h);
    if (isCulled(area))
        return;

    auto t = std::make_shared<JZImageTask>();
    t->clip = m_clip;
    t->image = img.data();
    t->area = area;
    m_layer->addTask(t);
}
