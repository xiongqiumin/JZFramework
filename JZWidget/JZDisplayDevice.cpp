#include "JZDisplayDevice.h"
#include "JZWidget.h"
#include "JZPainter.h"
#include "JZPainterEvent.h"

#include <algorithm>

JZDisplayDevice::JZDisplayDevice(void* display)
    : JZWidget(NoCreate())
    , m_layer(0, 0)
{
}

JZDisplayDevice::JZDisplayDevice(int w, int h)
    : JZWidget(NoCreate())
    , m_layer(w, h)
{
    setGeometry(0, 0, w, h);
    setOpaque(true);
}

JZDisplayDevice::~JZDisplayDevice()
{
}

void JZDisplayDevice::invalidate(const JZRect& rect)
{
    // 等价 lv_inv_area: 只把矩形并入脏区域, 不触发绘制。
    if (rect.isEmpty())
        return;
    m_dirtyRect = m_dirtyRect.united(rect);
}

void JZDisplayDevice::refresh()
{
    if (m_layer.buffer()->width() == 0 || m_layer.buffer()->height() == 0)
        return;
    if (m_dirtyRect.isEmpty())
        return;

    JZRect area = m_dirtyRect;
    m_dirtyRect = JZRect();   // 取走并清空脏区域

    // 1) 脏区域已通过 united() 合并 (等价 lv_refr_join_area)

    // 2) 铺底 -> 剔除遍历发 paintEvent(生成 draw_task) -> dispatch 混合
    m_layer.buffer()->clearRect(area.x(), area.y(), area.width(), area.height(), m_bg);

    std::vector<JZWidget*> list = cullList(this, area);

    m_layer.dispatch();      // 执行本帧 draw_task (裁剪到 area), 写进屏缓冲
    m_layer.clearTasks();

    flush();                  // 3) 递交硬件
}

void JZDisplayDevice::flush()
{
    // 默认空实现; 平台(如 SDL)子类重写, 把 frameBuffer() 拷到屏幕。
}

void* JZDisplayDevice::frameBuffer()
{
    return m_layer.buffer()->data();
}

int JZDisplayDevice::width() const
{
    return m_layer.buffer()->width();
}

int JZDisplayDevice::height() const
{
    return m_layer.buffer()->height();
}

void JZDisplayDevice::setBackgroundColor(const JZColor& c)
{
    m_bg = JZPixel{255, c.red(), c.green(), c.blue()};
}

JZColor JZDisplayDevice::backgroundColor() const
{
    return JZColor(m_bg.r, m_bg.g, m_bg.b);
}

// ==================== 剔除遍历 ====================
// 等价 LVGL lv_refr_get_top_obj: 找到"最上层完全不透明覆盖者", 只画它及其祖先链,
// 被覆盖的 widget 不发 paintEvent、不生成 draw_task。
// 这里用"前到后"累计不透明覆盖, 收集需要绘制的 widget, 最后反转成"后到前"绘制顺序。
// coverage 用 std::vector<JZRect> 保存多块已覆盖矩形 (正确性需要, 不能用单个 JZRect)。
void JZDisplayDevice::collectFrontToBack(JZWidget* w, const JZRect& area, std::vector<JZRect>& coverage,
                                         std::vector<JZWidget*>& out)
{
    JZRect wa = w->geometry().intersect(area);
    if (wa.isEmpty() || !w->isVisible())
        return;

    // 先处理子节点(前到后), 使上层子节点先贡献覆盖, 再判断本节点(本节点在子节点之后)
    const auto& ch = w->children();
    for (int i = (int)ch.size() - 1; i >= 0; --i) {
        auto* c = jzobject_cast<JZWidget*>(ch[i]);
        if (c)
            collectFrontToBack(c, area, coverage, out);
    }

    // 被已覆盖矩形完全包含 -> 剔除 (不发 paintEvent / 不生成 draw_task)
    bool covered = false;
    for (const JZRect& c : coverage) {
        if (wa.left()   >= c.left()  && wa.right()  <= c.right() &&
            wa.top()    >= c.top()   && wa.bottom() <= c.bottom()) {
            covered = true;
            break;
        }
    }
    if (!covered) {
        out.push_back(w);
        if (w->isOpaque())
            coverage.push_back(wa);
    }
}

std::vector<JZWidget*> JZDisplayDevice::cullList(JZWidget* root, const JZRect& area)
{
    std::vector<JZWidget*> frontToBack;
    std::vector<JZRect> coverage;
    collectFrontToBack(root, area, coverage, frontToBack);
    std::reverse(frontToBack.begin(), frontToBack.end()); // -> 后到前(正确 z 序)
    return frontToBack;
}
