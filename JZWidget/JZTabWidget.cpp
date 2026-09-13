#include "JZTabWidget.h"
#include "JZPainter.h"
#include "JZPainterEvent.h"
#include "JZFontMetrics.h"
#include <algorithm>

JZ_OBJECT_IMPL(JZTabWidget, JZWidget)

JZTabWidget::JZTabWidget(JZWidget* parent)
    : JZWidget(parent)
    , currentChanged(this)
{
    // 内部页面容器: 普通 JZWidget 子控件; 其背景绘制交给本控件, 方式是
    // 在该子控件上 installEventFilter, 由 eventFilter 拦截 paint 事件绘制。
    m_pageArea = new JZWidget(this);
    m_pageArea->installEventFilter(this);
    relayout();
}

// ==================== 数据 ====================

int JZTabWidget::addTab(JZWidget* page, const std::string& label)
{
    m_pages.push_back(page);
    m_labels.push_back(label);
    int idx = (int)m_pages.size() - 1;

    page->setParentWidget(m_pageArea);
    if (m_current < 0) {
        m_current = 0;                    // 首个页面成为当前页
    } else {
        page->setVisible(false);          // 其余页面默认隐藏, 切换时再显示
    }
    relayout();                           // 立即铺满页面容器 (hidden 页面也定几何)
    update();
    updateGeometry();
    return idx;
}

void JZTabWidget::removeTab(int index)
{
    if (index < 0 || index >= (int)m_pages.size())
        return;

    delete m_pages[index];                // 页面由本控件持有
    m_pages.erase(m_pages.begin() + index);
    m_labels.erase(m_labels.begin() + index);

    if ((int)m_pages.size() == 0) {
        m_current = -1;
    } else if (index < m_current) {
        --m_current;
    } else if (m_current >= (int)m_pages.size()) {
        m_current = (int)m_pages.size() - 1;
    }
    showCurrentPage();
    update();
    updateGeometry();
}

JZWidget* JZTabWidget::widget(int index) const
{
    if (index < 0 || index >= (int)m_pages.size())
        return nullptr;
    return m_pages[index];
}

int JZTabWidget::indexOf(JZWidget* page) const
{
    for (int i = 0; i < (int)m_pages.size(); ++i) {
        if (m_pages[i] == page)
            return i;
    }
    return -1;
}

int JZTabWidget::count() const
{
    return (int)m_pages.size();
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
    if (index < 0 || index >= (int)m_pages.size() || index == m_current)
        return;
    m_current = index;
    showCurrentPage();
    update();
    currentChanged.emit(index);
}

void JZTabWidget::showCurrentPage()
{
    for (int i = 0; i < (int)m_pages.size(); ++i)
        m_pages[i]->setVisible(i == m_current);
}

// ==================== 几何 ====================

void JZTabWidget::relayout()
{
    if (!m_pageArea)
        return;
    m_pageArea->setGeometry(0, kTabBarH, std::max(0, width()),
                            std::max(0, height() - kTabBarH));
    for (JZWidget* page : m_pages) {
        page->setGeometry(0, 0, m_pageArea->width(), m_pageArea->height());
    }
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

JZSize JZTabWidget::sizeHint() const
{
    int w = 0, h = 0;
    for (JZWidget* page : m_pages) {
        JZSize s = page->sizeHint();
        w = std::max(w, s.width());
        h = std::max(h, s.height());
    }
    return JZSize(w, h + kTabBarH);
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

    // 标签条背景 + 底部分隔线 (页面区背景见 eventFilter 对 m_pageArea 的绘制)
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

bool JZTabWidget::eventFilter(JZObject* watched, JZEvent* event)
{
    // 页面容器是普通 JZWidget, 没有自绘代码 —— 背景在这里画 (拦截其 paint 事件)
    if (watched == m_pageArea && event->type() == JZEvent_paint) {
        // 自建 painter: 与控件 paintEvent 同一约定(裁剪由 painter 构造设定)
        JZPainter p(m_pageArea);
        p.fillRect(0, 0, m_pageArea->width(), m_pageArea->height(),
                   JZColor(0xe8, 0xe8, 0xe8));
        return false;   // 继续默认派发 (默认 paintEvent 为空, 无副作用)
    }
    return JZWidget::eventFilter(watched, event);
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
