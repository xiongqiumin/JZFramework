#include "JZTabWidget.h"
#include "JZPainter.h"
#include "JZFontMetrics.h"
#include <algorithm>

JZ_OBJECT_IMPL(JZTabWidget, JZWidget)

JZTabWidget::JZTabWidget(JZWidget* parent)
    : JZWidget(parent)
    , currentChanged(this)
{
    m_stack = new JZStackedWidget(this);
    relayout();
}

// ==================== 数据 ====================

int JZTabWidget::addTab(JZWidget* page, const std::string& label)
{
    page->setParentWidget(m_stack);       // 页面归内部 stacked 持有
    m_labels.push_back(label);
    int idx = m_stack->addWidget(page);   // 首个页面自动成为当前页
    if (m_current < 0)
        m_current = 0;
    update();
    updateGeometry();
    return idx;
}

void JZTabWidget::removeTab(int index)
{
    if (index < 0 || index >= (int)m_labels.size())
        return;

    m_labels.erase(m_labels.begin() + index);
    m_stack->removeWidget(index);         // 同时删除页面

    if (m_current >= (int)m_labels.size())
        m_current = (int)m_labels.size() - 1;
    m_stack->setCurrentIndex(m_current);
    update();
    updateGeometry();
}

JZWidget* JZTabWidget::widget(int index) const
{
    return m_stack ? m_stack->widget(index) : nullptr;
}

int JZTabWidget::indexOf(JZWidget* page) const
{
    return m_stack ? m_stack->indexOf(page) : -1;
}

int JZTabWidget::count() const
{
    return (int)m_labels.size();
}

std::string JZTabWidget::tabText(int index) const
{
    if (index < 0 || index >= (int)m_labels.size())
        return {};
    return m_labels[index];
}

// ==================== 当前页 ====================

int JZTabWidget::currentIndex() const
{
    return m_current;
}

void JZTabWidget::setCurrentIndex(int index)
{
    if (index < 0 || index >= (int)m_labels.size() || index == m_current)
        return;
    m_current = index;
    m_stack->setCurrentIndex(index);
    update();
    currentChanged.emit(index);
}

// ==================== 几何 ====================

void JZTabWidget::relayout()
{
    if (m_stack)
        m_stack->setGeometry(0, kTabBarH, std::max(0, width()),
                             std::max(0, height() - kTabBarH));
}

void JZTabWidget::resize(int w, int h)
{
    JZWidget::resize(w, h);
    relayout();
}

void JZTabWidget::setGeometry(int x, int y, int w, int h)
{
    JZWidget::setGeometry(x, y, w, h);
    relayout();
}

void JZTabWidget::setGeometry(const JZRect& r)
{
    setGeometry(r.x(), r.y(), r.width(), r.height());
}

JZSize JZTabWidget::sizeHint() const
{
    JZSize s = m_stack ? m_stack->sizeHint() : JZSize(0, 0);
    return JZSize(s.width(), s.height() + kTabBarH);
}

// ==================== 绘制 ====================

int JZTabWidget::tabWidth(int index) const
{
    JZFont font(14);
    return std::max(48, JZFontMetrics::textWidth(m_labels[index], font) + 24);
}

void JZTabWidget::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width();

    // 标签条背景 + 底部分隔线
    p.fillRect(0, 0, w, kTabBarH, JZColor(0xf0, 0xf0, 0xf0));
    p.setPen(JZPen(JZColor(0xdd, 0xdd, 0xdd), 1));
    p.drawLine(0, kTabBarH - 1, w, kTabBarH - 1);

    JZFont font(14);
    p.setFont(font);
    int x = 0;
    for (int i = 0; i < (int)m_labels.size(); ++i) {
        int tw = tabWidth(i);
        bool sel = (i == m_current);

        if (sel) {
            // 选中标签: 白底 + 底部主题色下划线 (盖过分隔线)
            p.fillRect(x, 0, tw, kTabBarH - 1, JZColor(0xff, 0xff, 0xff));
            p.fillRect(x, kTabBarH - 3, tw, 3, JZColor(0x33, 0x66, 0xcc));
        }

        p.setPen(JZPen(sel ? JZColor(0x20, 0x20, 0x20) : JZColor(0x66, 0x66, 0x66), 1));
        p.drawText(x + 12, JZFontMetrics::centerY(font, kTabBarH), m_labels[i].c_str());
        x += tw;
    }
}

// ==================== 交互 ====================

int JZTabWidget::tabAt(int x) const
{
    int cursor = 0;
    for (int i = 0; i < (int)m_labels.size(); ++i) {
        int tw = tabWidth(i);
        if (x >= cursor && x < cursor + tw)
            return i;
        cursor += tw;
    }
    return -1;
}

void JZTabWidget::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() != JZMouseEvent::LeftButton ||
        event->localPos().y() >= kTabBarH) {
        JZWidget::mousePressEvent(event);
        return;
    }
    int idx = tabAt(event->localPos().x());
    if (idx >= 0)
        setCurrentIndex(idx);
}
