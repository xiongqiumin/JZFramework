#include <algorithm>
#include "JZDisplayDevice.h"
#include "JZWidget.h"
#include "JZPainter.h"
#include "JZPainterEvent.h"
#include "lvgl.h"
#include "JZStd.h"


/* LVGL 刷新回调: 屏幕 layer 开始绘制时, LVGL 会就主 draw_task 发一次
* LV_EVENT_DRAW_TASK_ADDED 事件。这里拿到正在刷新的 layer, 交给 JZLayer 直接画。 */
void drawEventCb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DRAW_TASK_ADDED)
        return;

    lv_draw_task_t* task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t* base = (lv_draw_dsc_base_t*)lv_draw_task_get_draw_dsc(task);
    if (base->part != LV_PART_MAIN)
        return;

    lv_layer_t* layer = base->layer;
    JZDisplayDevice* self = (JZDisplayDevice*)lv_event_get_user_data(e);
    self->layer()->redraw(layer);
}

//JZDisplayDevice
JZ_OBJECT_IMPL(JZDisplayDevice, JZWidget)

std::vector<JZDisplayDevice*> JZDisplayDevice::displayList;

void JZDisplayDevice::refresh()
{
    for (auto device : displayList)
        device->redraw();
}

JZDisplayDevice::JZDisplayDevice(lv_display_t* display)
{
    setDisplay(display);
}

JZDisplayDevice::JZDisplayDevice(int w, int h)
{    
#ifdef _WIN32
    if (displayList.size() == 0)
        lv_init();

    auto display = lv_windows_create_display(L"JZWidget", w, h, 100, false, false);
    lv_windows_acquire_pointer_indev(display);
    lv_windows_acquire_keypad_indev(display);
    lv_windows_acquire_encoder_indev(display);
#else
    auto display = lv_display_create(w, h);
#endif // _WIN32

    setDisplay(display);
}

void JZDisplayDevice::setDisplay(lv_display_t* display)
{
    displayList.push_back(this);
    m_disp = display;    

    // 把图层尺寸同步为显示分辨率 (drawTask 用 buffer 宽高当 HOR_RES/VER_RES)
    int w = lv_display_get_horizontal_resolution(m_disp);
    int h = lv_display_get_vertical_resolution(m_disp);

    // 挂载 LVGL 后端: 直接把 screen 的 layer 交给 JZLayer 绘制, 不创建任何 lv_obj。
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(scr, drawEventCb, LV_EVENT_DRAW_TASK_ADDED, this);

    m_visible = true;
    setGeometry(0, 0, w, h);
}

JZDisplayDevice::~JZDisplayDevice()
{
    vec_removeAll(displayList, this);
}

bool JZDisplayDevice::event(JZEvent *e)
{
    if(e->type() == JZEvent_paint)
    { 
        paintEvent((JZPainterEvent *)e);
        return true;
    }
    else
    {
        return JZObject::event(e);
    }
}

void JZDisplayDevice::paintEvent(JZPainterEvent *event)
{
    JZPainter pt(this);
    pt.fillRect(rect(), JZColor::White);
}

JZLayer *JZDisplayDevice::layer()
{
    return &m_layer;
}

void JZDisplayDevice::makeInvalidate(const JZRect &rc)
{
    if (m_invalidRect.isEmpty())
        m_invalidRect = rc;
    else
        m_invalidRect = m_invalidRect.intersect(rc);
}

void JZDisplayDevice::redraw()
{
    if (m_invalidRect.isEmpty())
        return;

    // 先保存脏区作为本次绘制的裁剪区/绘制区域, 随后清空已处理标记。
    JZRect clip = m_invalidRect;

    std::vector<JZWidget*> widget_list;
    std::vector<JZRect> coverage = { clip };
    collectFrontToBack(this, clip, coverage, widget_list);
    m_invalidRect.clear();

    std::reverse(widget_list.begin(), widget_list.end());
    // 统一创建 painter 并注入: 每个 widget 一次 paintEvent 对应一个 painter,
    // 其裁剪区设为脏区(绘制区域), 超出该区域的图元由 JZPainter 粗略剔除不提交。
    for (auto w : widget_list)
    {
        JZPainter painter(w);
        painter.setClip(clip);

        JZPainterEvent event;
        event.setPainter(&painter);
        event.setRect(clip);
        w->event(&event);
    }

    lv_obj_t *screen = lv_display_get_screen_active(m_disp);
    lv_obj_invalidate(screen);
}

// ==================== 剔除遍历 ====================
// 等价 LVGL lv_refr_get_top_obj: 找到"最上层完全不透明覆盖者", 只画它及其祖先链,
// 被覆盖的 widget 不发 paintEvent、不生成 draw_task。
// 这里用"前到后"累计不透明覆盖, 收集需要绘制的 widget, 最后反转成"后到前"绘制顺序。
// coverage 用 std::vector<JZRect> 保存多块已覆盖矩形 (正确性需要, 不能用单个 JZRect)。
void JZDisplayDevice::collectFrontToBack(JZWidget* w, const JZRect& area, std::vector<JZRect>& coverage,
                                         std::vector<JZWidget*>& out)
{
    JZRect wa = w->toGlobal(w->rect()).intersect(area);
    /*
    if (wa.isEmpty() || !w->isVisible())
        return;
    */

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
    /*if (!covered)*/ {
        out.push_back(w);
        coverage.push_back(wa);
    }
}