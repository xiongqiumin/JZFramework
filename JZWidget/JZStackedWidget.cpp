#include "JZStackedWidget.h"
#include "JZStd.h"
#include "lvgl.h"

JZ_OBJECT_IMPL(JZStackedWidget, JZWidget)

JZStackedWidget::JZStackedWidget(JZWidget* parent)
    : JZWidget(parent)
{
}

int JZStackedWidget::addWidget(JZWidget* w)
{
    if (!w) return -1;

    m_pages.push_back(w);
    w->setParentWidget(this);

    // page 铺满容器, 用百分比尺寸自动跟随容器大小
    lv_obj_t* h = w->handle();
    lv_obj_set_pos(h, 0, 0);
    lv_obj_set_size(h, LV_PCT(100), LV_PCT(100));

    int idx = (int)m_pages.size() - 1;
    if (m_current < 0)
        setCurrentIndex(0);
    else
        updateVisibility();
    return idx;
}

void JZStackedWidget::insertWidget(int index, JZWidget* w)
{
    if (!w) return;
    if (index < 0) index = 0;
    if (index > (int)m_pages.size()) index = (int)m_pages.size();

    m_pages.insert(m_pages.begin() + index, w);
    w->setParentWidget(this);

    lv_obj_t* h = w->handle();
    lv_obj_set_pos(h, 0, 0);
    lv_obj_set_size(h, LV_PCT(100), LV_PCT(100));

    // 在当前页之前插入, 当前索引顺移
    if (m_current >= index) m_current++;

    if (m_current < 0)
        setCurrentIndex(0);
    else
        updateVisibility();
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
