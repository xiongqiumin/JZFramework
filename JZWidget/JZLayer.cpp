#include "JZLayer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "lvgl.h"

#include "JZColor.h"

static lv_color_t toLvColor(const JZColor &c)
{
    return lv_color_make(c.red(), c.green(), c.blue());
}

JZLayer::JZLayer()
{
}

// LVGL v9: 在创建 draw task 前设置 layer->_clip_area, lv_draw_add_task 会把
// 当前 _clip_area 保存到新 task 里, 渲染时 draw unit 使用该 clip。
void JZLayer::applyClip(lv_layer_t *layer, const JZRect &clip)
{
    if (clip.isEmpty()) {
        // 不裁剪: 退化为整个 layer 缓冲区域
        layer->_clip_area = layer->buf_area;
    } else {
        layer->_clip_area.x1 = clip.left();
        layer->_clip_area.y1 = clip.top();
        layer->_clip_area.x2 = clip.right() - 1;
        layer->_clip_area.y2 = clip.bottom() - 1;
    }
}

void JZLayer::redraw(lv_layer_t *layer)
{
    for (auto &task : m_tasks) {
        applyClip(layer, task->clip);

        switch (task->type) {
        case JZDrawTask::Line: {
            auto* t = static_cast<JZLineTask*>(task.get());
            lv_draw_line_dsc_t dsc;
            lv_draw_line_dsc_init(&dsc);
            dsc.p1.x = (lv_value_precise_t)t->x1;
            dsc.p1.y = (lv_value_precise_t)t->y1;
            dsc.p2.x = (lv_value_precise_t)t->x2;
            dsc.p2.y = (lv_value_precise_t)t->y2;
            dsc.color = toLvColor(t->pen.color());
            dsc.width = t->pen.width() > 0 ? t->pen.width() : 1;
            dsc.opa   = t->opacity;
            lv_draw_line(layer, &dsc);
            break;
        }
        case JZDrawTask::Rect: {
            auto* t = static_cast<JZFillRectTask*>(task.get());
            lv_draw_fill_dsc_t dsc;
            lv_draw_fill_dsc_init(&dsc);
            dsc.color = toLvColor(t->color);
            dsc.opa   = t->opacity;
            dsc.radius = t->radius;
            lv_area_t a;
            a.x1 = t->area.left();
            a.y1 = t->area.top();
            a.x2 = t->area.right() - 1;
            a.y2 = t->area.bottom() - 1;
            lv_draw_fill(layer, &dsc, &a);
            break;
        }
        case JZDrawTask::Border: {
            auto* t = static_cast<JZBorderTask*>(task.get());
            lv_draw_border_dsc_t dsc;
            lv_draw_border_dsc_init(&dsc);
            dsc.color = toLvColor(t->pen.color());
            dsc.opa   = t->opacity;
            dsc.width = t->pen.width() > 0 ? t->pen.width() : 1;
            dsc.radius = t->radius;
            dsc.side = LV_BORDER_SIDE_FULL;
            lv_area_t a;
            a.x1 = t->area.left();
            a.y1 = t->area.top();
            a.x2 = t->area.right() - 1;
            a.y2 = t->area.bottom() - 1;
            lv_draw_border(layer, &dsc, &a);
            break;
        }
        case JZDrawTask::Arc: {
            auto* t = static_cast<JZArcTask*>(task.get());
            lv_draw_arc_dsc_t dsc;
            lv_draw_arc_dsc_init(&dsc);
            dsc.color = toLvColor(t->pen.color());
            dsc.opa   = t->opacity;
            dsc.width = t->pen.width() > 0 ? t->pen.width() : 1;
            dsc.center.x = t->arcCx;
            dsc.center.y = t->arcCy;
            dsc.radius = (uint16_t)t->arcRadius;
            // 内部角度已为 1/10 度; 整圆(3600)时 LVGL 需要一个完整的 360 度弧,
            // 故把 3600 折算为 360 以避免 LVGL 把 3600 当作超大角度。
            int start = t->arcStartAngle;
            int end = (t->arcEndAngle >= 3600) ? 360 : t->arcEndAngle;
            dsc.start_angle = (lv_value_precise_t)start;
            dsc.end_angle   = (lv_value_precise_t)end;
            dsc.rounded = t->arcRounded ? 1 : 0;
            lv_draw_arc(layer, &dsc);
            break;
        }
        case JZDrawTask::Text: {
            auto* t = static_cast<JZTextTask*>(task.get());
            if (t->text.empty())
                break;
            lv_draw_label_dsc_t dsc;
            lv_draw_label_dsc_init(&dsc);
            dsc.text = t->text.c_str();
            dsc.color = toLvColor(t->color);
            dsc.opa   = t->opacity;
            dsc.font = LV_FONT_DEFAULT;
            dsc.align = LV_TEXT_ALIGN_LEFT;
            lv_area_t a;
            a.x1 = t->area.left();
            a.y1 = t->area.top();
            a.x2 = t->area.right() - 1;
            a.y2 = t->area.bottom() - 1;
            lv_draw_label(layer, &dsc, &a);
            break;
        }
        case JZDrawTask::Image: {
            auto* t = static_cast<JZImageTask*>(task.get());
            if (!t->image || !t->image->dsc())
                break;
            lv_draw_image_dsc_t dsc;
            lv_draw_image_dsc_init(&dsc);
            dsc.src = t->image->dsc();   // 指向 lv_image_dsc_t (或文件路径)
            dsc.opa = t->opacity;
            lv_area_t a;
            a.x1 = t->area.left();
            a.y1 = t->area.top();
            a.x2 = t->area.right() - 1;
            a.y2 = t->area.bottom() - 1;
            lv_draw_image(layer, &dsc, &a);
            break;
        }
        case JZDrawTask::Changed:
            // 状态栈由 JZPainter 自身维护, 此处仅为绘制流中的序列化标记, 渲染无操作。
            break;
        case JZDrawTask::None:
        default:
            break;
        }
    }

    m_tasks.clear();
}
