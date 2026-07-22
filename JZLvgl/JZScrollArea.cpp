#include "JZScrollArea.h"
#include "lvgl.h"

JZ_OBJECT_IMPL(JZScrollArea, JZWidget)

// ============================================================================
// JZScrollArea
// ============================================================================
JZScrollArea::JZScrollArea(JZWidget* parent)
    : JZWidget(parent)
{
    lv_obj_set_style_pad_all(m_lv, 0, 0);
    lv_obj_set_style_radius(m_lv, 0, 0);
    lv_obj_add_flag(m_lv, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(m_lv, LV_DIR_ALL);
    lv_obj_set_scrollbar_mode(m_lv, LV_SCROLLBAR_MODE_AUTO);

    // 透明占位: 仅用于撑开滚动范围, 自身不绘制、不接收点击
    m_content = new JZWidget(this);
    lv_obj_set_style_pad_all(m_content->handle(), 0, 0);
    lv_obj_clear_flag(m_content->handle(), LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(m_content->handle(), LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(m_content->handle(), LV_OPA_TRANSP, 0);
}

JZScrollArea::~JZScrollArea()
{
}

void JZScrollArea::setScrollDirection(lv_dir_t dir)
{
    lv_obj_set_scroll_dir(m_lv, dir);
}

void JZScrollArea::setScrollBarPolicy(ScrollBarPolicy policy)
{
    lv_scrollbar_mode_t m;
    switch (policy) {
    case ScrollBarAlwaysOff: m = LV_SCROLLBAR_MODE_OFF;  break;
    case ScrollBarAlwaysOn:  m = LV_SCROLLBAR_MODE_ON;   break;
    default:                 m = LV_SCROLLBAR_MODE_AUTO; break;
    }
    lv_obj_set_scrollbar_mode(m_lv, m);
}

void JZScrollArea::setVerticalScrollBarPolicy(ScrollBarPolicy policy)
{
    setScrollBarPolicy(policy);
}

void JZScrollArea::setHorizontalScrollBarPolicy(ScrollBarPolicy policy)
{
    setScrollBarPolicy(policy);
}

void JZScrollArea::setContentSize(int w, int h)
{
    if (!m_content) return;
    if (w < 0) lv_obj_set_width(m_content->handle(), LV_PCT(100));
    else       lv_obj_set_width(m_content->handle(), w);
    if (h < 0) lv_obj_set_height(m_content->handle(), LV_PCT(100));
    else       lv_obj_set_height(m_content->handle(), h);
}

int JZScrollArea::contentWidth() const
{
    return m_content ? lv_obj_get_width(m_content->handle()) : 0;
}

int JZScrollArea::contentHeight() const
{
    return m_content ? lv_obj_get_height(m_content->handle()) : 0;
}

int JZScrollArea::scrollX() const
{
    return lv_obj_get_scroll_x(m_lv);
}

int JZScrollArea::scrollY() const
{
    return lv_obj_get_scroll_y(m_lv);
}

int JZScrollArea::viewportWidth() const
{
    return lv_obj_get_width(m_lv);
}

int JZScrollArea::viewportHeight() const
{
    return lv_obj_get_height(m_lv);
}

void JZScrollArea::scrollTo(int x, int y)
{
    lv_obj_scroll_to(m_lv, x, y, LV_ANIM_OFF);
}

void JZScrollArea::scrollToX(int x)
{
    lv_obj_scroll_to_x(m_lv, x, LV_ANIM_OFF);
}

void JZScrollArea::scrollToY(int y)
{
    lv_obj_scroll_to_y(m_lv, y, LV_ANIM_OFF);
}

void JZScrollArea::scrollToTop()
{
    lv_obj_scroll_to_y(m_lv, 0, LV_ANIM_OFF);
}

void JZScrollArea::scrollToBottom()
{
    lv_obj_scroll_to_y(m_lv, LV_COORD_MAX, LV_ANIM_OFF);
}

bool JZScrollArea::canScrollVertically() const
{
    return contentHeight() > viewportHeight();
}

void JZScrollArea::onLvEvent(lv_event_t* e)
{
    JZWidget::onLvEvent(e);
    if (lv_event_get_code(e) == LV_EVENT_SCROLL)
        onScroll();
}

void JZScrollArea::onScroll()
{
}
