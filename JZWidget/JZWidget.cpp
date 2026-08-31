#include "JZWidget.h"
#include "JZApplication.h"
#include "JZDisplayDevice.h"

#include <cassert>
#include <algorithm>

JZ_OBJECT_IMPL(JZWidget,JZObject)

JZWidget::JZWidget(JZWidget* parent)
    : JZObject(parent)
{
    m_minW = 0;
    m_minH = 0;
    m_maxW = INT_MAX;
    m_maxH = INT_MAX;

    m_horPolicy = Preferred;
    m_verPolicy = Preferred;

    m_x = 0;
    m_y = 0;
    m_width = 0;
    m_height = 0;

    if (parent)
    {
        auto device = jzobject_cast<JZDisplayDevice*>(parent);
        if(device)
            resize(device->width(), device->height());

        m_visible = device? false: true;
    }
    else
        m_visible = false;
}

JZWidget::~JZWidget()
{
}

void JZWidget::setParentWidget(JZWidget* parent)
{
    setParent(parent);
}

JZWidget* JZWidget::parentWidget() const
{
    JZWidget* w = jzobject_cast<JZWidget*>(m_parent);
    return w;
}

class JZDisplayDevice* JZWidget::display()
{
    JZWidget* p = (JZWidget*)this;
    while (p) {
        if (auto* d = jzobject_cast<JZDisplayDevice*>(p))
            return d;
        p = p->parentWidget();
    }
    return nullptr;
}

// ==================== 位置 ====================

JZPoint JZWidget::pos() const
{ return JZPoint(m_x, m_y); }

void JZWidget::move(int x, int y)
{
    m_x = x;
    m_y = y;
}

void JZWidget::move(const JZPoint& p)
{
    move(p.x(), p.y());
}

int JZWidget::x() const
{ 
    return m_x; 
}

int JZWidget::y() const
{ 
    return m_y; 
}

// ==================== 尺寸 ====================

JZSize JZWidget::size() const
{ 
    return JZSize(m_width, m_height); 
}

void JZWidget::resize(int w, int h)
{
    w = std::max(m_minW, std::min(w, m_maxW));
    h = std::max(m_minH, std::min(h, m_maxH));
    if (w == m_width && h == m_height)
        return;
    m_width = w;
    m_height = h;
    if (m_layout)
        m_layout->updateLayout();
}

void JZWidget::resize(const JZSize& s)
{
    resize(s.width(), s.height());
}

// ==================== 尺寸约束 ====================

void JZWidget::setFixedSize(int w, int h)
{
    m_minW = m_maxW = w;
    m_minH = m_maxH = h;
    resize(w, h);
}

void JZWidget::setFixedWidth(int w)
{
    m_minW = m_maxW = w;
    resize(w, height());
}

void JZWidget::setFixedHeight(int h)
{
    m_minH = m_maxH = h;
    resize(width(), h);
}

void JZWidget::setMinimumSize(int w, int h)
{
    m_minW = w;
    m_minH = h;
    resize(width(), height());
}

void JZWidget::setMinimumWidth(int w)
{
    m_minW = w;
    resize(width(), height());
}

void JZWidget::setMinimumHeight(int h)
{
    m_minH = h;
    resize(width(), height());
}

void JZWidget::setMaximumSize(int w, int h)
{
    m_maxW = w;
    m_maxH = h;
    resize(width(), height());
}

void JZWidget::setMaximumWidth(int w)
{
    m_maxW = w;
    resize(width(), height());
}

void JZWidget::setMaximumHeight(int h)
{
    m_maxH = h;
    resize(width(), height());
}

int JZWidget::minimumWidth() const
{ return m_minW; }

int JZWidget::minimumHeight() const
{ return m_minH; }

int JZWidget::maximumWidth() const
{ return m_maxW; }

int JZWidget::maximumHeight() const
{ return m_maxH; }

int JZWidget::width() const
{ return m_width; }

int JZWidget::height() const
{ return m_height; }

JZRect JZWidget::rect() const
{
    return JZRect(0, 0, m_width, m_height);
}

JZRect JZWidget::toGlobal(const JZRect &rect) const
{
    // 沿父链累加局部坐标, 把本地矩形换算到显示设备坐标系。
    // 注意: 仅累加 widget 自身的位置偏移 (m_x, m_y); 各祖先的尺寸/缩放不影响子节点原点,
    // 子节点的 geometry 已相对父 widget 的左上角 (0,0)。
    int gx = rect.x();
    int gy = rect.y();

    const JZWidget* w = this;
    while (w) {
        gx += w->m_x;
        gy += w->m_y;
        w = w->parentWidget();
    }
    return JZRect(gx, gy, rect.width(), rect.height());
}

// ==================== 尺寸策略 / baseSize ====================

JZWidget::SizePolicy JZWidget::horizontalPolicy() const
{ return m_horPolicy; }

JZWidget::SizePolicy JZWidget::verticalPolicy() const
{ return m_verPolicy; }

void JZWidget::setSizePolicy(SizePolicy h, SizePolicy v)
{
    m_horPolicy = h;
    m_verPolicy = v;
    updateGeometry();   // 策略影响父布局如何排本控件, 需向上级联
}

void JZWidget::setHorizontalPolicy(SizePolicy p)
{
    m_horPolicy = p;
    updateGeometry();
}

void JZWidget::setVerticalPolicy(SizePolicy p)
{
    m_verPolicy = p;
    updateGeometry();
}

JZSize JZWidget::sizeHint() const
{
    // 容器: 优先用布局计算的内容期望尺寸; 叶子控件由子类重写返回固有尺寸。
    if (m_layout)
        return m_layout->sizeHint();
    return JZSize(0, 0);
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
    if (m_visible == visible)
        return;

    m_visible = visible;
    update();
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
    return m_visible; 
}


// ==================== 布局 ====================

void JZWidget::setLayout(JZLayout* layout)
{
    if(m_layout)
        delete m_layout;
    
    m_layout = layout;
    if(m_layout)
    {
        m_layout->apply(this);  // 内部 updateLayout(): 重排自身子控件
        updateGeometry();       // 持有布局后 sizeHint 改变, 向上级联(父布局需重排)
    }
}

void JZWidget::updateLayout()
{
    if (m_layout)
        m_layout->updateLayout();
}

void JZWidget::updateGeometry()
{
    // 本控件尺寸特征改变(策略/约束/布局内容), 父布局将其作为 item 必须重排,
    // 再继续向上传播到整条祖先链。
    // 注意: 不在此重排自身子控件 —— 父布局经 setGeometry 设置本控件几何后,
    // resize() 会自动触发本控件 updateLayout(), 从而级联向下重排子控件。
    if (JZWidget* p = parentWidget())
    {
        if (p->m_layout)
            p->m_layout->updateLayout();
        p->updateGeometry();
    }
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
void JZWidget::invalidate(const JZRect &rc)
{
    auto device = this->display();
    if (!device)
        return;

    JZRect global_rc = toGlobal(rc);
    device->makeInvalidate(global_rc);
}

void JZWidget::update()
{
    invalidate(rect());
}

bool JZWidget::event(JZEvent* e)
{
    if (e->type() == JZEvent_paint)
    {
        paintEvent((JZPainterEvent*)e);
        return true;
    }

    if (e->type() == JZEvent_mouse)
    {
        JZMouseEvent* me = (JZMouseEvent*)e;
        switch (me->mouseType()) {
        case JZMouseEvent::Press:   mousePressEvent(me);   return true;
        case JZMouseEvent::Release: mouseReleaseEvent(me); return true;
        case JZMouseEvent::Move:    mouseMoveEvent(me);    return true;
        case JZMouseEvent::Wheel:   wheelEvent(me);        return true;
        }
        return true;
    }

    return JZObject::event(e);
}

void JZWidget::paintEvent(JZPainterEvent*)
{
}

void JZWidget::mousePressEvent(JZMouseEvent*)
{
}

void JZWidget::mouseReleaseEvent(JZMouseEvent*)
{
}

void JZWidget::mouseMoveEvent(JZMouseEvent*)
{
}

void JZWidget::wheelEvent(JZMouseEvent*)
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
