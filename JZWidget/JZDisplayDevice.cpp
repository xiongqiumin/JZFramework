#include "JZDisplayDevice.h"
#include "JZWidget.h"
#include "JZPainter.h"
#include "JZPainterEvent.h"

#include <algorithm>

#include "lvgl.h"

JZDisplayDevice::JZDisplayDevice(void* display)
    : JZWidget(NoCreate())
    , m_layer(0, 0)
{
    m_disp = reinterpret_cast<lv_display_t*>(display);
    if (m_disp) {
        // 把图层尺寸同步为显示分辨率 (drawTask 用 buffer 宽高当 HOR_RES/VER_RES)
        int w = lv_display_get_horizontal_resolution(m_disp);
        int h = lv_display_get_vertical_resolution(m_disp);
        m_layer.resize(w, h);

        // 挂载 LVGL 后端: 直接把 screen 的 layer 交给 JZLayer::drawTask 绘制,
        // 不创建任何 lv_obj (等价参考 demo 的 setup 部分)。
        lv_obj_t* scr = lv_screen_active();
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        lv_obj_add_flag(scr, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
        lv_obj_add_event_cb(scr, drawEventCb, LV_EVENT_DRAW_TASK_ADDED, this);
    }
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

/* LVGL 刷新回调: 屏幕 layer 开始绘制时, LVGL 会就主 draw_task 发一次
 * LV_EVENT_DRAW_TASK_ADDED 事件。这里拿到正在刷新的 layer, 交给 JZLayer 直接画。 */
void JZDisplayDevice::drawEventCb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DRAW_TASK_ADDED)
        return;

    lv_draw_task_t* task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t* base = (lv_draw_dsc_base_t*)lv_draw_task_get_draw_dsc(task);
    if (base->part != LV_PART_MAIN)
        return;

    lv_layer_t* layer = base->layer;
    JZDisplayDevice* self = (JZDisplayDevice*)lv_event_get_user_data(e);
    if (self)
        self->m_layer.drawTask(layer);
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
    // LVGL 后端: 直接驱动 LVGL 把屏幕全屏标脏并同步刷新; 刷新时 drawEventCb 调
    // JZLayer::drawTask() 把整屏画到正在刷新的 layer 上 (flush 由 LVGL 的 flush_cb 完成)。
    if (m_disp) {
        m_layer.nextFrame();
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(m_disp);
        return;
    }

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
