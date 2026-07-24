#include "JZWidget.h"
#include "JZApplication.h"
#include "JZDisplayDevice.h"

#include <cassert>
#include <algorithm>

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

class JZDisplayDevice* JZWidget::display() const
{
    JZObject* p = m_parent;
    while (p) {
        if (auto* d = jzobject_cast<JZDisplayDevice*>(p))
            return d;
        p = p->parent();
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
{ return m_x; }

int JZWidget::y() const
{ return m_y; }

// ==================== 尺寸 ====================

JZSize JZWidget::size() const
{ return JZSize(m_w, m_h); }

void JZWidget::resize(int w, int h)
{
    w = std::max(m_minW, std::min(w, m_maxW));
    h = std::max(m_minH, std::min(h, m_maxH));
    if (w == m_w && h == m_h)
        return;
    m_w = w;
    m_h = h;
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
{ return m_w; }

int JZWidget::height() const
{ return m_h; }

// ==================== 尺寸策略 / baseSize ====================

JZWidget::SizePolicy JZWidget::horizontalPolicy() const
{ return m_hPolicy; }

JZWidget::SizePolicy JZWidget::verticalPolicy() const
{ return m_vPolicy; }

void JZWidget::setSizePolicy(SizePolicy h, SizePolicy v)
{
    m_hPolicy = h;
    m_vPolicy = v;
    if (m_layout)
        m_layout->updateLayout();
}

void JZWidget::setHorizontalPolicy(SizePolicy p)
{
    m_hPolicy = p;
    if (m_layout)
        m_layout->updateLayout();
}

void JZWidget::setVerticalPolicy(SizePolicy p)
{
    m_vPolicy = p;
    if (m_layout)
        m_layout->updateLayout();
}

JZSize JZWidget::baseSize() const
{ return m_baseSize; }

void JZWidget::setBaseSize(const JZSize& s)
{
    m_baseSize = s;
    if (m_layout)
        m_layout->updateLayout();
}

void JZWidget::setBaseSize(int w, int h)
{
    setBaseSize(JZSize(w, h));
}

JZSize JZWidget::sizeHint() const
{
    if (!m_baseSize.isEmpty())
        return m_baseSize;
    return JZSize(m_w, m_h);
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
{ return m_visible; }

void JZWidget::setOpaque(bool opaque)
{
    m_opaque = opaque;
}

bool JZWidget::isOpaque() const
{ return m_opaque; }

// ==================== 布局 ====================

void JZWidget::setLayout(JZLayout* layout)
{
    if(m_layout)
        delete m_layout;
    
    m_layout = layout;
    if(m_layout)
        m_layout->apply(this);
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
{
    // 等价 lv_obj_invalidate: 只把矩形登记到显示设备的脏区域集合,
    // 不生成任何 paintEvent / draw_task (那些在刷新遍历时才产生)。
    if (auto* d = display())
        d->invalidate(geometry());
}

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
