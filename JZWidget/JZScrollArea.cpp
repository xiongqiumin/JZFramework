#include "JZScrollArea.h"

#include <algorithm>

#include "JZScrollBar.h"
#include "JZPainter.h"
#include "JZPainterEvent.h"
#include "JZMouseEvent.h"
#include "JZKeyEvent.h"
#include "JZResizeEvent.h"
#include "JZSignal.h"
#include "JZColor.h"

JZ_OBJECT_IMPL(JZScrollArea, JZWidget)

namespace {
// 集成滚动条主轴厚度 (与独立滚动条箭头尺寸一致, px)
const int kBarThickness = 16;
// 滚轮 / 滚动条单步滚动量 (px)
const int kScrollStep = 40;
}

JZScrollArea::JZScrollArea(JZWidget* parent)
    : JZWidget(parent)
{
    m_vbar = new JZScrollBar(JZScrollBar::Vertical, this);
    m_hbar = new JZScrollBar(JZScrollBar::Horizontal, this);
    m_vbar->setVisible(false);
    m_hbar->setVisible(false);
    m_vbar->setSingleStep(kScrollStep);
    m_hbar->setSingleStep(kScrollStep);

    // 滚动条被拖动/点轨道/滚轮 -> 值变化 -> 同步滚动偏移
    connect(&m_vbar->sigValueChanged, [this](int v) {
        if (m_scrollY == v)
            return;
        m_scrollY = v;
        if (m_widgetContent)
            m_widgetContent->move(-m_scrollX, -m_scrollY);
        update();
    });
    connect(&m_hbar->sigValueChanged, [this](int v) {
        if (m_scrollX == v)
            return;
        m_scrollX = v;
        if (m_widgetContent)
            m_widgetContent->move(-m_scrollX, -m_scrollY);
        update();
    });
}

JZScrollArea::~JZScrollArea()
{
}

// ----------------------------------------------------------------------------
// 滚动条策略
// ----------------------------------------------------------------------------
void JZScrollArea::setScrollDirection(int dir)
{
    // 兼容保留: 原 LVGL 风格方向开关已不再需要 —— 内容尺寸为 -1 的方向
    // (跟随视口) 本来就不会产生滚动条。Tree/List 等只传 ScrollBarAsNeeded。
    (void)dir;
}

void JZScrollArea::setScrollBarPolicy(ScrollBarPolicy policy)
{
    setVerticalScrollBarPolicy(policy);
    setHorizontalScrollBarPolicy(policy);
}

void JZScrollArea::setVerticalScrollBarPolicy(ScrollBarPolicy policy)
{
    if (m_vPolicy == policy)
        return;
    m_vPolicy = policy;
    layoutScrollBars();
    update();
}

void JZScrollArea::setHorizontalScrollBarPolicy(ScrollBarPolicy policy)
{
    if (m_hPolicy == policy)
        return;
    m_hPolicy = policy;
    layoutScrollBars();
    update();
}

JZScrollArea::ScrollBarPolicy JZScrollArea::verticalScrollBarPolicy() const
{
    return m_vPolicy;
}

JZScrollArea::ScrollBarPolicy JZScrollArea::horizontalScrollBarPolicy() const
{
    return m_hPolicy;
}

JZScrollBar* JZScrollArea::verticalScrollBar() const
{
    return m_vbar;
}

JZScrollBar* JZScrollArea::horizontalScrollBar() const
{
    return m_hbar;
}

// ----------------------------------------------------------------------------
// 内容控件 (Qt QScrollArea::setWidget)
// ----------------------------------------------------------------------------
void JZScrollArea::setWidget(JZWidget* content)
{
    if (!content || content == m_widgetContent)
        return;

    // 替换旧内容: Qt 语义, 旧内容控件被接管删除
    if (m_widgetContent) {
        m_widgetContent->setParentWidget(nullptr);
        delete m_widgetContent;
        m_widgetContent = nullptr;
    }

    if (!m_viewport) {
        // 懒创建内部视口: 真实子控件, 裁剪所有越界子内容, 背景由本滚动区经
        // installEventFilter 绘制 (与 JZTabWidget 内部 JZWidget + 过滤器同套路)
        m_viewport = new JZWidget(this);
        m_viewport->setClipChildren(true);
        m_viewport->installEventFilter(this);
    }

    m_viewport->setVisible(true);
    m_widgetContent = content;
    m_widgetContent->setParentWidget(m_viewport);
    m_widgetContent->setVisible(true);   // Qt 语义: 无父构造的内容默认不可见, setWidget 后须显式显示
    m_widgetContent->move(0, 0);

    // 未显式定尺寸时按 sizeHint 初始化 (之后外部可自行 resize, 滚动范围随之更新)
    if (m_widgetContent->width() <= 0 && m_widgetContent->height() <= 0) {
        JZSize sh = m_widgetContent->sizeHint();
        m_widgetContent->resize(std::max(0, sh.width()), std::max(0, sh.height()));
    }

    m_scrollX = 0;
    m_scrollY = 0;
    layoutScrollBars();
    update();
}

JZWidget* JZScrollArea::widget() const
{
    return m_widgetContent;
}

JZWidget* JZScrollArea::viewport() const
{
    return m_viewport;
}

// ----------------------------------------------------------------------------
// 内容总尺寸 (模型 B: owner-draw 虚拟内容)
// ----------------------------------------------------------------------------
void JZScrollArea::setContentSize(int w, int h)
{
    m_contentW = w;
    m_contentH = h;
    layoutScrollBars();
    update();
}

int JZScrollArea::contentWidth() const
{
    if (m_widgetContent)
        return m_widgetContent->width();
    return m_contentW < 0 ? viewportWidth() : m_contentW;
}

int JZScrollArea::contentHeight() const
{
    if (m_widgetContent)
        return m_widgetContent->height();
    return m_contentH < 0 ? viewportHeight() : m_contentH;
}

// ----------------------------------------------------------------------------
// 滚动状态
// ----------------------------------------------------------------------------
int JZScrollArea::scrollX() const
{
    return m_scrollX;
}

int JZScrollArea::scrollY() const
{
    return m_scrollY;
}

int JZScrollArea::maxScrollX() const
{
    int cw = contentWidth();
    return std::max(0, cw - viewportWidth());
}

int JZScrollArea::maxScrollY() const
{
    int ch = contentHeight();
    return std::max(0, ch - viewportHeight());
}

int JZScrollArea::viewportWidth() const
{
    if (m_viewport)
        return m_viewport->width();
    int w = std::max(0, width());
    int t = m_vbar && m_vbar->isVisible() ? kBarThickness : 0;
    return std::max(0, w - t);
}

int JZScrollArea::viewportHeight() const
{
    if (m_viewport)
        return m_viewport->height();
    int h = std::max(0, height());
    int t = m_hbar && m_hbar->isVisible() ? kBarThickness : 0;
    return std::max(0, h - t);
}

void JZScrollArea::scrollTo(int x, int y)
{
    m_scrollX = x;
    m_scrollY = y;
    layoutScrollBars();
    update();
}

void JZScrollArea::scrollToX(int x)
{
    m_scrollX = x;
    layoutScrollBars();
    update();
}

void JZScrollArea::scrollToY(int y)
{
    m_scrollY = y;
    layoutScrollBars();
    update();
}

void JZScrollArea::scrollToTop()
{
    m_scrollX = 0;
    m_scrollY = 0;
    layoutScrollBars();
    update();
}

void JZScrollArea::scrollToBottom()
{
    m_scrollY = 0x7fffffff;   // layoutScrollBars 内按 maxScrollY 收窄
    m_scrollX = 0;
    layoutScrollBars();
    update();
}

bool JZScrollArea::canScrollVertically() const
{
    if (m_vPolicy == ScrollBarAlwaysOff)
        return false;
    int maxY = (m_widgetContent || m_contentH >= 0)
                   ? maxScrollY()
                   : 0;
    return maxY > 0;
}

bool JZScrollArea::canScrollHorizontally() const
{
    if (m_hPolicy == ScrollBarAlwaysOff)
        return false;
    int maxX = (m_widgetContent || m_contentW >= 0)
                   ? maxScrollX()
                   : 0;
    return maxX > 0;
}

JZSize JZScrollArea::sizeHint() const
{
    if (m_layout)
        return JZSize(120, 80);
    return JZWidget::sizeHint();
}

// ----------------------------------------------------------------------------
// 布局滚动条 / 视口 / 内容控件
// ----------------------------------------------------------------------------
void JZScrollArea::layoutScrollBars()
{
    const int t = kBarThickness;
    const int w = std::max(0, width());
    const int h = std::max(0, height());

    // 内容原始尺寸 (模型 A 取内容控件几何, 模型 B 取上报内容尺寸)
    const bool widgetMode = (m_widgetContent != nullptr);
    int cw = widgetMode ? m_widgetContent->width()
                        : (m_contentW < 0 ? w : m_contentW);
    int ch = widgetMode ? m_widgetContent->height()
                        : (m_contentH < 0 ? h : m_contentH);
    cw = std::max(0, cw);
    ch = std::max(0, ch);

    // 可见性: 策略 + 内容是否溢出视口 (交叉收缩需迭代 2 次)
    bool vVis = (m_vPolicy == ScrollBarAlwaysOn) ||
                (m_vPolicy == ScrollBarAsNeeded && ch > h);
    bool hVis = (m_hPolicy == ScrollBarAlwaysOn) ||
                (m_hPolicy == ScrollBarAsNeeded && cw > w);
    for (int i = 0; i < 2; i++) {
        if (m_vPolicy == ScrollBarAsNeeded)
            vVis = ch > std::max(0, h - (hVis ? t : 0));
        if (m_hPolicy == ScrollBarAsNeeded)
            hVis = cw > std::max(0, w - (vVis ? t : 0));
    }
    if (m_vPolicy == ScrollBarAlwaysOff) vVis = false;
    if (m_hPolicy == ScrollBarAlwaysOff) hVis = false;

    const int vw = std::max(0, w - (vVis ? t : 0));
    const int vh = std::max(0, h - (hVis ? t : 0));

    // 滚动范围与收窄
    int maxX = 0;
    int maxY = 0;
    if (widgetMode || m_contentW >= 0)
        maxX = std::max(0, cw - vw);
    if (widgetMode || m_contentH >= 0)
        maxY = std::max(0, ch - vh);
    m_scrollX = std::max(0, std::min(m_scrollX, maxX));
    m_scrollY = std::max(0, std::min(m_scrollY, maxY));

    // 视口几何
    if (m_viewport)
        m_viewport->setGeometry(0, 0, vw, vh);

    // 垂直滚动条 (占右带宽; 有水平滚动条时在其上端止步)
    m_vbar->setVisible(vVis);
    if (vVis) {
        m_vbar->setGeometry(w - t, 0, t, std::max(0, h - (hVis ? t : 0)));
        m_vbar->setRange(0, maxY);
        m_vbar->setPageStep(std::max(1, vh));
        m_vbar->setValue(m_scrollY);
    }

    // 水平滚动条 (占底部带宽)
    m_hbar->setVisible(hVis);
    if (hVis) {
        m_hbar->setGeometry(0, h - t, std::max(0, w - (vVis ? t : 0)), t);
        m_hbar->setRange(0, maxX);
        m_hbar->setPageStep(std::max(1, vw));
        m_hbar->setValue(m_scrollX);
    }

    // 内容控件跟随滚动偏移平移到视口内
    if (m_widgetContent)
        m_widgetContent->move(-m_scrollX, -m_scrollY);
}

// ----------------------------------------------------------------------------
// 事件过滤: 绘制内部视口背景
// ----------------------------------------------------------------------------
bool JZScrollArea::eventFilter(JZObject* watched, JZEvent* event)
{
    // 视口背景: 内容未覆盖的区域露出白底 (模型 A), 与独立滚动区灰底区分开
    if (watched == m_viewport && event->type() == JZEvent_paint && m_widgetContent) {
        JZPainterEvent* pe = (JZPainterEvent*)event;
        if (JZPainter* p = pe->painter())
            p->fillRect(0, 0, m_viewport->width(), m_viewport->height(), JZColor::White);
        return false;   // 默认 (空) paintEvent 继续派发, 无副作用
    }
    return JZWidget::eventFilter(watched, event);
}

// ----------------------------------------------------------------------------
// 事件
// ----------------------------------------------------------------------------
void JZScrollArea::paintEvent(JZPainterEvent* event)
{
    (void)event;
    // 每次重绘前刷新滚动条 (内容控件可能被外部改尺寸, 无需额外通知)
    layoutScrollBars();

    JZPainter p(this);
    int w = width(), h = height();
    // 滚动区底色 (浅灰), 模型 B 的子类会整体盖住, 模型 A 由 viewport 白底盖住内部
    p.fillRect(0, 0, w, h, JZColor(0xf5, 0xf5, 0xf5));
}

void JZScrollArea::wheelEvent(JZMouseEvent* event)
{
    if (event->wheelDelta() != 0) {
        const int dir = (event->wheelDelta() > 0) ? -1 : 1;   // 上滚 -> 内容上移
        if (canScrollVertically()) {
            scrollToY(m_scrollY + dir * kScrollStep);
            event->accept();
            return;
        }
        if (canScrollHorizontally()) {
            scrollToX(m_scrollX + dir * kScrollStep);
            event->accept();
            return;
        }
    }
    JZWidget::wheelEvent(event);
}

// 键盘滚动: PageUp/Down 翻页, Home/End 到顶/底, 方向键小步滚。
// List/Tree 的未处理键默认分支会委托到这里 (它们自身已消费方向键做项选择)。
void JZScrollArea::keyPressEvent(JZKeyEvent* event)
{
    const bool canV = canScrollVertically();
    const bool canH = canScrollHorizontally();
    bool handled = false;

    switch (event->key()) {
    case JZKey::Key_Home:
        if (canV) { m_scrollY = 0;         handled = true; }
        else if (canH) { m_scrollX = 0;    handled = true; }
        break;
    case JZKey::Key_End:
        if (canV) { m_scrollY = 0x7fffffff; handled = true; }   // layoutScrollBars 内收窄
        else if (canH) { m_scrollX = 0x7fffffff; handled = true; }
        break;
    case JZKey::Key_PageUp:                                            // PageUp
        if (canV) { m_scrollY -= viewportHeight(); handled = true; }
        else if (canH) { m_scrollX -= viewportWidth(); handled = true; }
        break;
    case JZKey::Key_PageDown:                                             // PageDown
        if (canV) { m_scrollY += viewportHeight(); handled = true; }
        else if (canH) { m_scrollX += viewportWidth(); handled = true; }
        break;
    case JZKey::Key_Up:
        if (canV) { m_scrollY -= kScrollStep; handled = true; }
        break;
    case JZKey::Key_Down:
        if (canV) { m_scrollY += kScrollStep; handled = true; }
        break;
    case JZKey::Key_Left:
        if (canH) { m_scrollX -= kScrollStep; handled = true; }
        break;
    case JZKey::Key_Right:
        if (canH) { m_scrollX += kScrollStep; handled = true; }
        break;
    default:
        break;
    }

    if (handled) {
        layoutScrollBars();
        update();
        event->accept();
        return;
    }
    JZWidget::keyPressEvent(event);
}

void JZScrollArea::resizeEvent(JZResizeEvent* event)
{
    JZWidget::resizeEvent(event);
    layoutScrollBars();
}
