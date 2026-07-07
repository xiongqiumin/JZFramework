#include "JZLvglWidget.h"
#include <cassert>

JZ_OBJECT_IMPL(JZLvglWidget,JZObject)

JZLvglWidget::JZLvglWidget(JZLvglWidget* parent)
    : JZObject(parent)
{
    auto* plv = parent ? parent->handle() : nullptr;
    auto obj = lv_obj_create(plv);
    setLvglObject(obj);
    lv_obj_set_user_data(m_lv, this);
    lv_obj_add_event_cb(m_lv, lvEventCallback, LV_EVENT_ALL, nullptr);
}

JZLvglWidget::JZLvglWidget(lv_create_fn_t create, JZLvglWidget* parent)
    : JZObject(parent)
{
    assert(create);
    auto* plv = parent ? parent->handle() : nullptr;
    auto obj = create(plv);
    setLvglObject(obj);    
}

JZLvglWidget::JZLvglWidget(NoCreate, JZLvglWidget* parent)
{
    m_lv = nullptr;
}

JZLvglWidget::~JZLvglWidget()
{
    if (m_lv) {
        lv_obj_delete(m_lv);
    }
}

void JZLvglWidget::setLvglObject(lv_obj_t* obj)
{
    m_lv = obj;
    lv_obj_set_user_data(m_lv, this);
    lv_obj_add_event_cb(m_lv, lvEventCallback, LV_EVENT_ALL, nullptr);
}

void JZLvglWidget::setParentWidget(JZLvglWidget* parent)
{
    setParent(parent);
    auto* plv = parent ? parent->handle() : nullptr;
    lv_obj_set_parent(m_lv, plv);
}

JZLvglWidget* JZLvglWidget::parentWidget() const
{
    JZLvglWidget* w = jzobject_cast<JZLvglWidget*>(m_parent);
    return w;
}

void JZLvglWidget::setLayout(const JZLvglLayout& layout)
{
    m_layout = layout;
    m_layout.apply(m_lv);
}

void JZLvglWidget::update()
{
    lv_obj_invalidate(m_lv);
}

void JZLvglWidget::lvEventCallback(lv_event_t* e)
{
    auto* lvObj = (lv_obj_t*)lv_event_get_current_target(e);
    auto* obj = (JZLvglWidget*)lv_obj_get_user_data(lvObj);
    if (!obj) return;

    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        obj->m_lv = nullptr;
        return;
    }

    obj->onLvEvent(e);
}

void JZLvglWidget::onLvEvent(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_KEY) {
        JZKeyEvent ke(e);
        onKeyPressed(&ke);
    }
    else if (code == LV_EVENT_DRAW_MAIN) {
        JZPainter p(e);
        paintEvent(&p);
    }
}

void JZLvglWidget::onKeyPressed(JZKeyEvent* event)
{
}

void JZLvglWidget::paintEvent(JZPainter* painter)
{
}

void JZLvglWidget::setProperty(const std::string& name, const JZVariant& value)
{
    int v = value.toInt();
    if (name == "x")        lv_obj_set_x(m_lv, (lv_coord_t)v);
    else if (name == "y")   lv_obj_set_y(m_lv, (lv_coord_t)v);
    else if (name == "width")   lv_obj_set_width(m_lv, (lv_coord_t)v);
    else if (name == "height")  lv_obj_set_height(m_lv, (lv_coord_t)v);
    else if (name == "opacity") lv_obj_set_style_opa(m_lv, (lv_opa_t)v, LV_PART_MAIN);
    else JZObject::setProperty(name, value);
}

const JZVariant& JZLvglWidget::property(const std::string& name) const
{
    if (name == "x")        { static JZVariant v; v = (int)lv_obj_get_x(m_lv); return v; }
    if (name == "y")        { static JZVariant v; v = (int)lv_obj_get_y(m_lv); return v; }
    if (name == "width")    { static JZVariant v; v = (int)lv_obj_get_width(m_lv); return v; }
    if (name == "height")   { static JZVariant v; v = (int)lv_obj_get_height(m_lv); return v; }
    if (name == "opacity")  { static JZVariant v; v = (int)lv_obj_get_style_opa(m_lv, LV_PART_MAIN); return v; }
    return JZObject::property(name);
}
