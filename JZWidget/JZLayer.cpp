#include "JZLayer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "lvgl.h"

JZLayer::JZLayer(int w, int h)
    : m_buf(w, h)
{
}

void JZLayer::resize(int w, int h)
{
    m_buf.resize(w, h);
}

/* 直接按 LVGL 的格式绘制到 layer (等价参考 demo 的 draw_event_cb)。
 * 这里不创建任何 lv_obj, 所有内容都用 lv_draw_* 画到屏幕上正在刷新的 layer 上。 */
void JZLayer::drawTask(lv_layer_t* layer)
{
    int32_t HOR_RES = m_buf.width();
    int32_t VER_RES = m_buf.height();

    /* ---- 1. Full-screen background ---- */
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_hex(0x0f1320);
    bg.bg_opa = LV_OPA_COVER;
    lv_area_t full = {0, 0, HOR_RES - 1, VER_RES - 1};
    lv_draw_rect(layer, &bg, &full);

    /* ---- 2. Title ---- */
    lv_draw_label_dsc_t title;
    lv_draw_label_dsc_init(&title);
    title.color = lv_color_hex(0x4fc3f7);
    title.font = &lv_font_montserrat_24;
    title.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t title_a = {0, 12, HOR_RES - 1, 48};
    title.text = "LVGL - draw directly on the layer (no widgets)";
    lv_draw_label(layer, &title, &title_a);

    /* ---- 3. Left panel ---- */
    lv_draw_rect_dsc_t panel;
    lv_draw_rect_dsc_init(&panel);
    panel.bg_color = lv_color_hex(0x1b2433);
    panel.bg_opa = LV_OPA_COVER;
    panel.radius = 16;
    panel.border_color = lv_color_hex(0x2e3a4f);
    panel.border_width = 2;
    lv_area_t panel_a = {40, 80, 360, 560};
    lv_draw_rect(layer, &panel, &panel_a);

    /* ---- 4. Animated gauge (arc) ---- */
    int32_t sweep = 135 + (int32_t)(135 * (1.0f + sinf((float)m_frame * 0.05f))); /* 135..405 */

    lv_draw_arc_dsc_t arc_bg;
    lv_draw_arc_dsc_init(&arc_bg);
    arc_bg.color = lv_color_hex(0x223047);
    arc_bg.width = 22;
    arc_bg.center.x = 200;
    arc_bg.center.y = 210;
    arc_bg.radius = 90;
    arc_bg.start_angle = 135;
    arc_bg.end_angle = 135 + 270;
    lv_draw_arc(layer, &arc_bg);

    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = lv_color_hex(0x4fc3f7);
    arc.width = 22;
    arc.center.x = 200;
    arc.center.y = 210;
    arc.radius = 90;
    arc.start_angle = 135;
    arc.end_angle = sweep;
    arc.rounded = 1;
    lv_draw_arc(layer, &arc);

    static char buf[32];
    int32_t pct = (int32_t)((sweep - 135) * 100 / 270);
    snprintf(buf, sizeof(buf), "%d%%", (int)pct);
    lv_draw_label_dsc_t val;
    lv_draw_label_dsc_init(&val);
    val.color = lv_color_white();
    val.font = &lv_font_montserrat_24;
    val.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t val_a = {arc_bg.center.x - 60, arc_bg.center.y - 18,
                       arc_bg.center.x + 60, arc_bg.center.y + 18};
    val.text = buf;
    lv_draw_label(layer, &val, &val_a);

    /* ---- 5. Animated progress bar ---- */
    int32_t pw = 40 + (int32_t)(260 * (0.5f + 0.5f * sinf((float)m_frame * 0.03f)));
    lv_area_t bar_bg_a = {40, 380, 360, 410};
    lv_draw_rect_dsc_t bar_bg;
    lv_draw_rect_dsc_init(&bar_bg);
    bar_bg.bg_color = lv_color_hex(0x0c1018);
    bar_bg.bg_opa = LV_OPA_COVER;
    bar_bg.radius = LV_RADIUS_CIRCLE;
    lv_draw_rect(layer, &bar_bg, &bar_bg_a);

    lv_area_t bar_a = {40, 380, 40 + pw, 410};
    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.bg_color = lv_color_hex(0x66bb6a);
    bar.bg_opa = LV_OPA_COVER;
    bar.radius = LV_RADIUS_CIRCLE;
    lv_draw_rect(layer, &bar, &bar_a);

    /* ---- 6. Bouncing dot ---- */
    int32_t cx = 450 + (int32_t)(300 * sinf((float)m_frame * 0.04f));
    int32_t cy = 220 + (int32_t)(150 * cosf((float)m_frame * 0.06f));
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = lv_color_hex(0xff7043);
    dot.bg_opa = LV_OPA_COVER;
    dot.radius = LV_RADIUS_CIRCLE;
    lv_area_t dot_a = {cx - 16, cy - 16, cx + 16, cy + 16};
    lv_draw_rect(layer, &dot, &dot_a);

    /* ---- 7. Right info panel + frame counter ---- */
    lv_draw_rect_dsc_t panel2;
    lv_draw_rect_dsc_init(&panel2);
    panel2.bg_color = lv_color_hex(0x1b2433);
    panel2.bg_opa = LV_OPA_COVER;
    panel2.radius = 16;
    panel2.border_color = lv_color_hex(0x2e3a4f);
    panel2.border_width = 2;
    lv_area_t panel2_a = {440, 80, 760, 560};
    lv_draw_rect(layer, &panel2, &panel2_a);

    static char fbuf[64];
    snprintf(fbuf, sizeof(fbuf), "Frame: %d", (int)m_frame);
    lv_draw_label_dsc_t info;
    lv_draw_label_dsc_init(&info);
    info.color = lv_color_hex(0xeceff1);
    info.font = &lv_font_montserrat_16;
    info.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t info_a = {440, 120, 760, 160};
    info.text = fbuf;
    lv_draw_label(layer, &info, &info_a);

    lv_draw_label_dsc_t hint;
    lv_draw_label_dsc_init(&hint);
    hint.color = lv_color_hex(0x90a4ae);
    hint.font = &lv_font_montserrat_12;
    hint.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t hint_a = {440, 180, 760, 220};
    hint.text = "Everything drawn with lv_draw_* on the layer";
    lv_draw_label(layer, &hint, &hint_a);
}

void JZLayer::dispatch()
{
    JZRect bufBounds = m_buf.bounds();
    for (const JZDrawTask& t : m_tasks) {
        JZRect a = t.area.intersect(t.clip);
        a = a.intersect(bufBounds);
        if (a.isEmpty())
            continue;

        if (t.type == JZDrawTaskType::Rect) {
            m_buf.fillRect(a.x(), a.y(), a.width(), a.height(), t.color);
        } else if (t.type == JZDrawTaskType::Border) {
            int bw = std::max(1, t.borderWidth);
            int x = a.x(), y = a.y(), w = a.width(), h = a.height();
            m_buf.fillRect(x, y, w, bw, t.color);                                 // top
            m_buf.fillRect(x, y + h - bw, w, bw, t.color);                        // bottom
            m_buf.fillRect(x, y, bw, h, t.color);                                 // left
            m_buf.fillRect(x + w - bw, y, bw, h, t.color);                        // right
        }
    }
}
