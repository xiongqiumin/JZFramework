#include "JZWidget.h"
#include "JZGuiApplication.h"
#include <cassert>

JZ_OBJECT_IMPL(JZWidget,JZObject)

JZWidget::JZWidget(JZWidget* parent)
    : JZObject(parent)
{
    auto* plv = parent ? parent->handle() : lv_screen_active();
    auto obj = lv_obj_create(plv);
    setLvglObject(obj);
}

JZWidget::JZWidget(lv_create_fn_t create, JZWidget* parent)
    : JZObject(parent)
{
    assert(create);
    auto* plv = parent ? parent->handle() : lv_screen_active();
    auto obj = create(plv);
    setLvglObject(obj);    
}

JZWidget::JZWidget(NoCreate, JZWidget* parent)
{
    m_lv = nullptr;
}

JZWidget::~JZWidget()
{
    delete m_layout;

    // 顶层 widget: 删除对应 screen（级联删除 m_lv）
    if (m_lv) {
        lv_obj_delete(m_lv);
    }

    // 无 screen 剩余 → 退出
    if (!m_parent && jzApp) {
        auto* guiApp = dynamic_cast<JZGUIApplication*>(jzApp);
        if (guiApp && guiApp->screenCount() == 0)
            guiApp->quit();
    }
}

lv_obj_t* JZWidget::handle() const
{
    return m_lv;
}

void JZWidget::setLvglObject(lv_obj_t* obj)
{
    m_lv = obj;
    lv_obj_set_user_data(m_lv, this);
    lv_obj_add_event_cb(m_lv, lvEventCallback, LV_EVENT_ALL, nullptr);
}

void JZWidget::setParentWidget(JZWidget* parent)
{
    setParent(parent);
    auto* plv = parent ? parent->handle() : nullptr;
    lv_obj_set_parent(m_lv, plv);
}

JZWidget* JZWidget::parentWidget() const
{
    JZWidget* w = jzobject_cast<JZWidget*>(m_parent);
    return w;
}

// ==================== 位置 ====================

JZPoint JZWidget::pos() const
{
    return JZPoint(lv_obj_get_x(m_lv), lv_obj_get_y(m_lv));
}

void JZWidget::move(int x, int y)
{
    lv_obj_set_pos(m_lv, x, y);
}

void JZWidget::move(const JZPoint& p)
{
    move(p.x(), p.y());
}

int JZWidget::x() const
{
    return lv_obj_get_x(m_lv);
}

int JZWidget::y() const
{
    return lv_obj_get_y(m_lv);
}

// ==================== 尺寸 ====================

JZSize JZWidget::size() const
{
    return JZSize(lv_obj_get_width(m_lv), lv_obj_get_height(m_lv));
}

void JZWidget::resize(int w, int h)
{
    lv_obj_set_size(m_lv, w, h);
    // 顶层 widget: 同步调整 screen 大小
    if (!m_parent) {
        lv_obj_t* scr = lv_obj_get_screen(m_lv);
        if (scr) lv_obj_set_size(scr, w, h);
    }
}

void JZWidget::resize(const JZSize& s)
{
    resize(s.width(), s.height());
}

int JZWidget::width() const
{
    return lv_obj_get_width(m_lv);
}

int JZWidget::height() const
{
    return lv_obj_get_height(m_lv);
}

// ==================== 几何 ====================

JZRect JZWidget::geometry() const
{
    return JZRect(x(), y(), width(), height());
}

void JZWidget::setGeometry(int x, int y, int w, int h)
{
    resize(w, h);
    move(x, y);
}

void JZWidget::setGeometry(const JZRect& r)
{
    setGeometry(r.x(), r.y(), r.width(), r.height());
}

// ==================== 可见性 ====================

void JZWidget::setVisible(bool visible)
{
    if (visible)
        lv_obj_remove_flag(m_lv, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(m_lv, LV_OBJ_FLAG_HIDDEN);
}

void JZWidget::show()
{
    setVisible(true);
    { 
        JZShowEvent se; 
        showEvent(&se); 
    }
}

void JZWidget::hide()
{
    setVisible(false);
}

bool JZWidget::isVisible() const
{
    return !lv_obj_has_flag(m_lv, LV_OBJ_FLAG_HIDDEN);
}

// ==================== 布局 ====================

void JZWidget::setLayout(JZLayout* layout)
{
    delete m_layout;
    m_layout = layout;
    m_layout->apply(m_lv);
}

JZLayout* JZWidget::layout() const
{
    return m_layout;
}

// ==================== 焦点 ====================

void JZWidget::setFocus()
{
    lv_group_focus_obj(m_lv);
}

bool JZWidget::hasFocus() const
{
    return lv_obj_has_state(m_lv, LV_STATE_FOCUSED);
}

// ==================== 重绘 ====================

void JZWidget::update()
{
    lv_obj_invalidate(m_lv);
}

// ==================== 事件 ====================

void JZWidget::lvEventCallback(lv_event_t* e)
{
    auto* lvObj = (lv_obj_t*)lv_event_get_current_target(e);
    auto* obj = (JZWidget*)lv_obj_get_user_data(lvObj);
    if (!obj) return;

    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        { JZCloseEvent ce; obj->closeEvent(&ce); }
        obj->m_lv = nullptr;
        return;
    }

    obj->onLvEvent(e);
}

void JZWidget::onLvEvent(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);

    switch (code) {
    case LV_EVENT_KEY:
        { JZKeyEvent ke(e); onKeyPressed(&ke); }
        break;
    case LV_EVENT_DRAW_MAIN:
        { m_paintLayer = lv_event_get_layer(e);
          JZPainterEvent pe; paintEvent(&pe);
          m_paintLayer = nullptr; }
        break;
    case LV_EVENT_SIZE_CHANGED:
        { 
            JZSize newSz(lv_obj_get_width(m_lv), lv_obj_get_height(m_lv));
            JZSize oldSz;
            lv_area_t* prev = (lv_area_t*)lv_event_get_param(e);
            if (prev) 
            oldSz = JZSize(lv_area_get_width(prev), lv_area_get_height(prev));

            JZResizeEvent re(newSz, oldSz);
            resizeEvent(&re); 
            break;
        }
    }
}

void JZWidget::onKeyPressed(JZKeyEvent* event)
{
}

void JZWidget::paintEvent(JZPainterEvent*)
{
}

void JZWidget::showEvent(JZShowEvent*)
{
}

void JZWidget::closeEvent(JZCloseEvent*)
{
}

void JZWidget::resizeEvent(JZResizeEvent*)
{
}

// ==================== 属性 ====================

void JZWidget::setProperty(const std::string& name, const JZVariant& value)
{
    int v = value.toInt();
    if (name == "x")        lv_obj_set_x(m_lv, (lv_coord_t)v);
    else if (name == "y")   lv_obj_set_y(m_lv, (lv_coord_t)v);
    else if (name == "width")   lv_obj_set_width(m_lv, (lv_coord_t)v);
    else if (name == "height")  lv_obj_set_height(m_lv, (lv_coord_t)v);
    else if (name == "opacity") lv_obj_set_style_opa(m_lv, (lv_opa_t)v, LV_PART_MAIN);
    else JZObject::setProperty(name, value);
}

JZVariant JZWidget::property(const std::string& name) const
{
    if (name == "x")        { JZVariant v; v = (int)lv_obj_get_x(m_lv); return v; }
    if (name == "y")        { JZVariant v; v = (int)lv_obj_get_y(m_lv); return v; }
    if (name == "width")    { JZVariant v; v = (int)lv_obj_get_width(m_lv); return v; }
    if (name == "height")   { JZVariant v; v = (int)lv_obj_get_height(m_lv); return v; }
    if (name == "opacity")  { JZVariant v; v = (int)lv_obj_get_style_opa(m_lv, LV_PART_MAIN); return v; }
    return JZObject::property(name);
}
