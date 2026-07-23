#include "JZWidget.h"
#include "JZApplication.h"

#include <cassert>

JZ_OBJECT_IMPL(JZWidget,JZObject)

JZWidget::JZWidget(JZWidget* parent)
    : JZObject(parent)
{}

JZWidget::JZWidget(JZCreateFn create, JZWidget* parent)
    : JZObject(parent)
{
    assert(create);
    auto* plv = parent->handle();
    auto obj = create(plv);
    setLvglObject(obj);    
}

JZWidget::JZWidget(NoCreate, JZWidget* parent)
{
    m_lv = nullptr;
}

JZWidget::~JZWidget()
{}

void* JZWidget::handle() const
{
    return m_lv;
}

void JZWidget::setLvglObject(void* obj)
{}

void JZWidget::setParentWidget(JZWidget* parent)
{}

JZWidget* JZWidget::parentWidget() const
{
    JZWidget* w = jzobject_cast<JZWidget*>(m_parent);
    return w;
}

// ==================== 位置 ====================

JZPoint JZWidget::pos() const
{ return {}; }

void JZWidget::move(int x, int y)
{}

void JZWidget::move(const JZPoint& p)
{
    move(p.x(), p.y());
}

int JZWidget::x() const
{ return 0; }

int JZWidget::y() const
{ return 0; }

// ==================== 尺寸 ====================

JZSize JZWidget::size() const
{ return {}; }

void JZWidget::resize(int w, int h)
{}

void JZWidget::resize(const JZSize& s)
{
    resize(s.width(), s.height());
}

int JZWidget::width() const
{ return 0; }

int JZWidget::height() const
{ return 0; }

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
{ return; }

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
{ return false; }

// ==================== 布局 ====================

void JZWidget::setLayout(JZLayout* layout)
{
    if(m_layout)
        delete m_layout;
    
    m_layout = layout;
    if(m_layout)
        m_layout->apply(m_lv);
}

JZLayout* JZWidget::layout() const
{
    return m_layout;
}

// ==================== 焦点 ====================

void JZWidget::setFocus()
{ return; }

bool JZWidget::hasFocus() const
{ return false; }

// ==================== 重绘 ====================

void JZWidget::update()
{ return; }

// ==================== 事件 ====================

void JZWidget::lvEventCallback(void* e)
{ return; }

void JZWidget::onLvEvent(void* e)
{}

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
{ return; }

JZVariant JZWidget::property(const std::string& name) const
{ return {}; }
