#include "JZStackedWidget.h"
#include "JZStd.h"
#include "JZPainter.h"
#include <algorithm>


JZ_OBJECT_IMPL(JZStackedWidget, JZWidget)

JZStackedWidget::JZStackedWidget(JZWidget* parent)
    : JZWidget(parent)
{
}

int JZStackedWidget::addWidget(JZWidget* w)
{
    m_pages.push_back(w);
    int idx = (int)m_pages.size() - 1;

    // 页面铺满 stacked widget 区域
    w->setGeometry(0, 0, width(), height());

    if (m_current < 0) {
        m_current = 0;
        w->setVisible(true);
    } else {
        w->setVisible(idx == m_current);
    }

    updateGeometry();   // 页面数变化 -> sizeHint 变化 -> 向上级联重排
    return idx;
}

void JZStackedWidget::insertWidget(int index, JZWidget* w)
{
    if (index < 0 || index > (int)m_pages.size())
        index = (int)m_pages.size();
    m_pages.insert(m_pages.begin() + index, w);

    w->setGeometry(0, 0, width(), height());

    if (m_current < 0) {
        m_current = 0;
        w->setVisible(true);
    } else {
        w->setVisible(index == m_current);
        if (index <= m_current)
            m_current++;
    }

    updateGeometry();
}

void JZStackedWidget::removeWidget(int index)
{
    if (index < 0 || index >= (int)m_pages.size()) return;

    JZWidget* w = m_pages[index];
    m_pages.erase(m_pages.begin() + index);

    // 安全: JZObject 析构会自动把 w 从本容器 m_childs 摘掉, 不会双重释放
    delete w;

    if (m_current == index) {
        m_current = -1;
        if (!m_pages.empty())
            setCurrentIndex(0);
    }
    else if (m_current > index) {
        m_current--;
    }
}

JZWidget* JZStackedWidget::widget(int index) const
{
    if (index < 0 || index >= (int)m_pages.size()) return nullptr;
    return m_pages[index];
}

int JZStackedWidget::count() const
{
    return (int)m_pages.size();
}

int JZStackedWidget::indexOf(JZWidget* widget) const
{
    for (int i = 0; i < (int)m_pages.size(); i++) {
        if (m_pages[i] == widget) return i;
    }
    return -1;
}

int JZStackedWidget::currentIndex() const
{
    return m_current;
}

void JZStackedWidget::setCurrentIndex(int index)
{
    if (index < 0 || index >= (int)m_pages.size()) return;
    if (index == m_current) return;
    m_current = index;
    updateVisibility();
}

JZWidget* JZStackedWidget::currentWidget() const
{
    return widget(m_current);
}

void JZStackedWidget::updateVisibility()
{
    for (int i = 0; i < (int)m_pages.size(); i++)
        m_pages[i]->setVisible(i == m_current);
}

void JZStackedWidget::setGeometry(int x, int y, int w, int h)
{
    JZWidget::setGeometry(x, y, w, h);
    // 所有页面叠放在本控件区域内, 随本控件尺寸同步缩放
    for (auto* p : m_pages)
        p->setGeometry(0, 0, w, h);
}

JZSize JZStackedWidget::sizeHint() const
{
    JZSize s(0, 0);
    for (auto* p : m_pages) {
        JZSize ps = p->sizeHint();
        s = JZSize(std::max(s.width(), ps.width()), std::max(s.height(), ps.height()));
    }
    return s;
}

void JZStackedWidget::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width(), h = height();
    p.setBrush(JZBrush(JZColor(0xe8, 0xe8, 0xe8)));
    p.setPen(JZPen(JZColor(0xe8, 0xe8, 0xe8), 1));
    p.drawRect(0, 0, w, h);
}
