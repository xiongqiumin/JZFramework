#include "JZTabWidget.h"
#include "JZPainter.h"
#include "JZPainterEvent.h"
#include "JZFontMetrics.h"
#include "JZMouseEvent.h"
#include "JZPen.h"
#include "JZColor.h"
#include <algorithm>

JZ_OBJECT_IMPL(JZTabWidget, JZWidget)

namespace {
// 选中标签那条主题色边(贴页面区的一侧)的粗细与配色
const int kAccentThickness = 3;
const JZColor kAccentColor(0x33, 0x66, 0xcc);
const JZColor kTabBarBg(0xf0, 0xf0, 0xf0);
const JZColor kPageBg(0xe8, 0xe8, 0xe8);
const JZColor kSeparator(0xdd, 0xdd, 0xdd);
const JZColor kTextOn(0x20, 0x20, 0x20);
const JZColor kTextOff(0x66, 0x66, 0x66);
const int kLabelPadding = 12;   // 标签文本两端留白
}

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

JZWidget* JZTabWidget::currentWidget() const
{
    return widget(m_current);
}

void JZTabWidget::setCurrentWidget(JZWidget* page)
{
    int i = indexOf(page);
    if (i < 0)
        return;                 // 非本控件的页: 静默忽略(Qt 同款)
    setCurrentIndex(i);         // 已经是当前页时 setCurrentIndex 自己会 no-op
}

void JZTabWidget::showCurrentPage()
{
    for (int i = 0; i < (int)m_pages.size(); ++i)
        m_pages[i]->setVisible(i == m_current);
}

// ==================== 标签条方向 ====================

void JZTabWidget::setTabPosition(TabPosition pos)
{
    if (m_tabPosition == pos)
        return;
    m_tabPosition = pos;
    relayout();          // 页面区换到另一侧
    update();
    updateGeometry();    // sizeHint 随方向变(条厚算在宽还是高上)
}

// ==================== 几何 ====================

int JZTabWidget::tabThickness() const
{
    return kTabBarThickness;
}

int JZTabWidget::tabLength(int index) const
{
    JZFont font(14);
    return std::max(kTabMinLength, JZFontMetrics::textWidth(m_labels[index], font) + 2 * kLabelPadding);
}

JZRect JZTabWidget::tabBarRect() const
{
    const int w = width();
    const int h = height();
    const int th = tabThickness();

    switch (m_tabPosition) {
    case North: return JZRect(0, 0, w, th);
    case South: return JZRect(0, std::max(0, h - th), w, th);
    case West:  return JZRect(0, 0, th, h);
    default:    return JZRect(std::max(0, w - th), 0, th, h);   // East
    }
}

JZRect JZTabWidget::tabRect(int index) const
{
    // 沿标签条方向累加前面每个标签的长度
    int pos = 0;
    for (int i = 0; i < index && i < (int)m_labels.size(); ++i)
        pos += tabLength(i);

    const int len = (index >= 0 && index < (int)m_labels.size()) ? tabLength(index) : 0;
    const int th = tabThickness();
    const int w = width();
    const int h = height();

    switch (m_tabPosition) {
    case North: return JZRect(pos, 0, len, th);
    case South: return JZRect(pos, h - th, len, th);
    case West:  return JZRect(0, pos, th, len);
    default:    return JZRect(w - th, pos, th, len);   // East
    }
}

JZRect JZTabWidget::contentRect() const
{
    const int w = width();
    const int h = height();
    const int th = tabThickness();

    switch (m_tabPosition) {
    case North: return JZRect(0, th, w, std::max(0, h - th));
    case South: return JZRect(0, 0, w, std::max(0, h - th));
    case West:  return JZRect(th, 0, std::max(0, w - th), h);
    default:    return JZRect(0, 0, std::max(0, w - th), h);   // East
    }
}

void JZTabWidget::relayout()
{
    if (!m_pageArea)
        return;

    const JZRect c = contentRect();
    m_pageArea->setGeometry(c.x(), c.y(), c.width(), c.height());
    for (JZWidget* page : m_pages) {
        page->setGeometry(0, 0, m_pageArea->width(), m_pageArea->height());
    }
}

void JZTabWidget::resizeEvent(JZResizeEvent* event)
{
    JZWidget::resizeEvent(event);
    relayout();   // 页面区/标签条按新尺寸重算(页签占位只有它自己算得出)
}

JZSize JZTabWidget::sizeHint() const
{
    int w = 0, h = 0;
    for (JZWidget* page : m_pages) {
        JZSize s = page->sizeHint();
        w = std::max(w, s.width());
        h = std::max(h, s.height());
    }

    // 标签条占的那一侧算进期望尺寸
    const int th = tabThickness();
    if (m_tabPosition == North || m_tabPosition == South)
        return JZSize(w, h + th);
    return JZSize(w + th, h);
}

// ==================== 绘制 ====================

// 画一个标签的文字: North/South 横排; West/East 用画布矩阵转 90° 竖排。
void JZTabWidget::paintTabText(JZPainter& p, const JZRect& r, const std::string& text, bool sel)
{
    JZFont font(14);
    p.setPen(JZPen(sel ? kTextOn : kTextOff, 1));

    if (m_tabPosition == North || m_tabPosition == South) {
        p.drawText(r.x() + kLabelPadding,
                   r.y() + JZFontMetrics::centerY(font, r.height()), text.c_str());
        return;
    }

    // 竖排: 把画布转 90° 再按横排的方式画(矩阵会下发到后端, 字形跟着转)。
    //   West(左): 转 -90°(屏幕逆时针) ⇒ 文字从下往上读, 起点在标签左下
    //   East(右): 转 +90°(屏幕顺时针) ⇒ 文字从上往下读, 起点在标签左上
    // 基线沿条厚方向居中: 用 centerY(font, 条厚) 得到"在厚度里居中"的基线位置。
    const int thickness = tabThickness();
    const int baseline = JZFontMetrics::centerY(font, thickness);

    p.save();
    if (m_tabPosition == West) {
        p.translate(r.x() + baseline, r.y() + r.height() - kLabelPadding);
        p.rotate(-90);
    } else {
        p.translate(r.x() + thickness - baseline, r.y() + kLabelPadding);
        p.rotate(90);
    }
    p.drawText(0, 0, text.c_str());
    p.restore();
}

void JZTabWidget::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    const int w = width();
    const int h = height();
    const int th = tabThickness();
    const JZRect bar = tabBarRect();

    // 标签条背景 + 与页面区之间的分隔线 (页面区背景见 eventFilter 对 m_pageArea 的绘制)
    p.fillRect(bar, kTabBarBg);
    p.setPen(JZPen(kSeparator, 1));
    switch (m_tabPosition) {
    case North: p.drawLine(0, th - 1, w, th - 1); break;
    case South: p.drawLine(0, h - th, w, h - th); break;
    case West:  p.drawLine(th - 1, 0, th - 1, h); break;
    default:    p.drawLine(w - th, 0, w - th, h); break;   // East
    }

    JZFont font(14);
    p.setFont(font);

    for (int i = 0; i < (int)m_labels.size(); ++i) {
        const JZRect r = tabRect(i);
        const bool sel = (i == m_current);

        if (sel) {
            // 选中标签: 白底 + 贴页面区那一侧的主题色边
            p.fillRect(r, JZColor(0xff, 0xff, 0xff));
            switch (m_tabPosition) {
            case North:
                p.fillRect(r.x(), r.y() + r.height() - kAccentThickness, r.width(), kAccentThickness, kAccentColor);
                break;
            case South:
                p.fillRect(r.x(), r.y(), r.width(), kAccentThickness, kAccentColor);
                break;
            case West:
                p.fillRect(r.x() + r.width() - kAccentThickness, r.y(), kAccentThickness, r.height(), kAccentColor);
                break;
            default:   // East
                p.fillRect(r.x(), r.y(), kAccentThickness, r.height(), kAccentColor);
                break;
            }
        }

        paintTabText(p, r, m_labels[i], sel);
    }
}

bool JZTabWidget::eventFilter(JZObject* watched, JZEvent* event)
{
    // 页面容器是普通 JZWidget, 没有自绘代码 —— 背景在这里画 (拦截其 paint 事件)
    if (watched == m_pageArea && event->type() == JZEvent_paint) {
        // 自建 painter: 与控件 paintEvent 同一约定(裁剪由 painter 构造设定)
        JZPainter p(m_pageArea);
        p.fillRect(0, 0, m_pageArea->width(), m_pageArea->height(), kPageBg);
        return false;   // 继续默认派发 (默认 paintEvent 为空, 无副作用)
    }
    return JZWidget::eventFilter(watched, event);
}

// ==================== 交互 ====================

bool JZTabWidget::inTabBar(const JZPoint& pt) const
{
    return tabBarRect().contains(pt);
}

int JZTabWidget::tabAt(int pos) const
{
    int cursor = 0;
    for (int i = 0; i < (int)m_labels.size(); ++i) {
        int len = tabLength(i);
        if (pos >= cursor && pos < cursor + len)
            return i;
        cursor += len;
    }
    return -1;
}

void JZTabWidget::mousePressEvent(JZMouseEvent* event)
{
    const JZPoint pt = event->localPos();
    if (event->button() != JZMouseEvent::LeftButton || !inTabBar(pt)) {
        JZWidget::mousePressEvent(event);
        return;
    }

    // North/South 沿 x 找; West/East 沿 y 找
    const int pos = (m_tabPosition == North || m_tabPosition == South) ? pt.x() : pt.y();
    int idx = tabAt(pos);
    if (idx >= 0)
        setCurrentIndex(idx);
}
