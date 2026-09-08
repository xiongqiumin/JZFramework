#include <cassert>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "JZWidget.h"
#include "JZApplication.h"
#include "JZDisplayDevice.h"
#include "JZScreen.h"
#include "JZWindow.h"

#include "JZTimer.h"
#include "JZPainter.h"
#include "JZPen.h"
#include "JZColor.h"
#include "JZFont.h"
#include "JZFontMetrics.h"

// 内部: 自绘多行悬浮小面板 (仿 Qt 的 tooltip 窗口)。文件级, 不对外暴露。
class JZToolTipWidget : public JZWidget
{
    JZ_OBJECT_DECLARE(JZToolTipWidget)
public:
    explicit JZToolTipWidget(JZWidget* parent = nullptr)
        : JZWidget(parent) {}

    // tip 用 '\n' 分行; 按文字尺寸自适应面板
    void setTip(const JZString& tip)
    {
        std::vector<std::string> lines;
        std::string utf8 = tip.toStdString();
        std::string cur;
        for (char c : utf8) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        lines.push_back(cur);

        JZFont f(kFontSize);
        int lineH = JZFontMetrics::textHeight(f);
        int w = 0;
        for (const auto& s : lines)
            w = std::max(w, JZFontMetrics::textWidth(s, f));

        setFixedSize(w + kPadX * 2, (int)lines.size() * lineH + kPadY * 2);
        m_lines = lines;
        update();
    }

protected:
    void paintEvent(JZPainterEvent*) override
    {
        JZPainter p(this);
        p.fillRect(0, 0, width(), height(), JZColor(0x2b, 0x2b, 0x30));
        p.setPen(JZPen(JZColor(0x60, 0x60, 0x68), 1));
        p.drawRect(0, 0, width(), height());

        JZFont f(kFontSize);
        int lineH = JZFontMetrics::textHeight(f);
        p.setFont(f);
        p.setPen(JZPen(JZColor(0xf0, 0xf0, 0xf0), 1));

        int y = kPadY;
        for (const auto& s : m_lines) {
            p.drawText(kPadX, y + JZFontMetrics::centeredTextTop(f, lineH), s.c_str());
            y += lineH;
        }
    }

private:
    enum { kFontSize = 12, kPadX = 8, kPadY = 6 };
    std::vector<std::string> m_lines;
};
JZ_OBJECT_IMPL(JZToolTipWidget, JZWidget)

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
    m_visible = true;
}

void JZWidget::setOpaque(int opaque)
{
    m_opaque = opaque;
}

int JZWidget::opaque() const
{
    return m_opaque;
}

JZWidget::~JZWidget()
{
    // 1) 摘除"本控件被外部布局持有"的关系: 防止布局 items 继续持有悬垂指针。
    //    逐个复制遍历: removeWidget 会修改 m_layoutOwners。
    std::vector<JZLayout*> owners = m_layoutOwners;
    for (JZLayout* l : owners)
    {
        if (l)
            l->removeWidget(this);
    }
    m_layoutOwners.clear();

    // 2) 释放本控件自有的布局。布局不拥有子控件 (只持有引用), 子控件仍由
    //    对象树在随后的 ~JZObject 中统一析构; 此处先解绑可避免双重释放。
    if (m_layout)
    {
        delete m_layout;
        m_layout = nullptr;
    }
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

class JZWindow* JZWidget::window()
{
    JZWidget* p = (JZWidget*)this;
    while (p)
    {
        if (p->inherits("JZWindow"))
            return (JZWindow*)p;

        p = p->parentWidget();
    }
    return nullptr;
}

class JZWindow* JZWidget::window() const
{
    // window() 不修改对象状态, const 重载复用非 const 版本
    return const_cast<JZWidget*>(this)->window();
}

// ==================== 位置 ====================

JZPoint JZWidget::pos() const
{ return JZPoint(m_x, m_y); }

void JZWidget::move(int x, int y)
{
    if (m_x == x && m_y == y)
        return;
    // 旧位置先并入脏区: 后台缓冲持久不清, 不重绘旧位会留残影(背景没清空)
    invalidate(rect());
    m_x = x;
    m_y = y;
    update();   // 新位置并入脏区
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
    // 旧尺寸先并入脏区: 缩小时腾出的区域需重绘, 否则后台缓冲残影
    invalidate(rect());
    const JZSize oldSize(m_width, m_height);
    m_width = w;
    m_height = h;

    // 尺寸已生效后先通知自身 (子类在此按新宽高重排内容/裁剪滚动范围),
    // 再向下传播: 自身有布局则重排子控件; 子控件经 setGeometry->resize
    // 尺寸变化时会各自再收到 resizeEvent, 形成完整的级联通知链。
    JZResizeEvent ev(size(), oldSize);
    resizeEvent(&ev);

    if (m_layout)
        m_layout->updateLayout();
    update();   // 新尺寸并入脏区
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
{
    // 有效最小宽度 = max(显式设置的 m_minW, 布局推导的 minimumSizeHint 宽度)。
    // 与 Qt 一致: 带布局的容器无需 setMinimumWidth 也有贴合内容的下限。
    return std::max(m_minW, minimumSizeHint().width());
}

int JZWidget::minimumHeight() const
{
    return std::max(m_minH, minimumSizeHint().height());
}

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
// ==================== 坐标映射 ====================

JZPoint JZWidget::mapTo(JZWidget* parent, const JZPoint& pos) const
{
    // accumulate offsets from this up to (excluding) parent
    JZPoint result = pos;
    const JZWidget* w = this;
    while (w && w != parent)
    {
        result += JZPoint(w->x(), w->y());
        w = w->parentWidget();
    }
    return result;
}

JZPoint JZWidget::mapToGlobal(const JZPoint& pos) const
{
    // 与 Qt 一致: 返回真实屏幕逻辑坐标。
    // 先换算到所属窗口的局部坐标, 再 ×scale -> ClientToScreen -> ÷scale 得到屏幕坐标。
    JZWindow* win = window();
    if (!win || !win->nativeHandle() || !win->device())
        return mapTo(win, pos);   // 无原生窗口: 退化为控件树内累加坐标
    JZPoint inWin = mapTo(win, pos);
    float s = win->device()->scale();
    POINT pt = { (int)std::lround(inWin.x() * s), (int)std::lround(inWin.y() * s) };
    ClientToScreen((HWND)win->nativeHandle(), &pt);
    return JZPoint((int)std::lround(pt.x / s), (int)std::lround(pt.y / s));
}

JZRect JZWidget::mapTo(JZWidget* parent, const JZRect& rc) const
{
    JZPoint tl = mapTo(parent, rc.topLeft());
    return JZRect(tl.x(), tl.y(), rc.width(), rc.height());
}

JZRect JZWidget::mapToGlobal(const JZRect& rc) const
{
    JZPoint tl = mapToGlobal(rc.topLeft());
    return JZRect(tl.x(), tl.y(), rc.width(), rc.height());
}

// ==================== 抓图 ====================

JZImage JZWidget::grab()
{
    JZWindow* win = window();
    if (!win || !win->nativeHandle() || !win->device())
        return JZImage();
    if (width() <= 0 || height() <= 0)
        return JZImage();

    // 抓整幅客户区 (物理像素), 再按控件矩形裁剪
    const JZImage full = JZScreen::grabWindow(win->nativeHandle());
    if (full.isNull())
        return JZImage();

    const std::shared_ptr<JZImageData> fullData = full.data();
    const uint8_t* client = fullData->pixels();
    const int cw = full.width();
    const int ch = full.height();

    // 逻辑 -> 物理: 控件相对窗口客户区的偏移与尺寸均按 DPI 缩放
    const float s = win->device()->scale();
    const JZPoint off = mapTo(win, JZPoint(0, 0));
    const int x = (int)std::lround(off.x() * s);
    const int y = (int)std::lround(off.y() * s);
    const int w = (int)std::lround(width() * s);
    const int h = (int)std::lround(height() * s);

    // 与客户区求交 (控件可能部分在窗口外, 或缩放取整差 1px)
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(cw, x + w), y1 = std::min(ch, y + h);
    if (x1 <= x0 || y1 <= y0)
        return JZImage();

    const int rw = x1 - x0, rh = y1 - y0;
    std::vector<uint8_t> rgba((size_t)rw * rh * 4);
    for (int row = y0; row < y1; row++)
    {
        memcpy(&rgba[(size_t)(row - y0) * rw * 4],
               &client[(size_t)(row * cw + x0) * 4],
               (size_t)rw * 4);
    }
    return JZImage::fromRGBA(rgba.data(), rw, rh);
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
    // 容器: 优先用布局计算的内容期望尺寸;
    // 叶子控件: 无显式 hint 时以当前尺寸为期望 (手动 resize 的尺寸在布局中生效)。
    if (m_layout)
        return m_layout->sizeHint();

    return JZSize(0, 0);
}

JZSize JZWidget::minimumSizeHint() const
{
    // 参考 Qt QWidget::minimumSizeHint: 有布局时取布局的 minimumSizeHint
    // (子控件 minimum 尺寸之和); 无布局返回 0×0, 即不主动施加下限。
    if (m_layout)
        return m_layout->minimumSizeHint();
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
    // 可见性改变会直接影响布局对空间的占用 (隐藏项被布局剔除), 需向上级联重排:
    // updateGeometry 经父控件布局对本控件所在 item 重新排布, 并继续向上传播。
    updateGeometry();
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
    {
        JZHideEvent he;
        hideEvent(&he);
    }
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

// ==================== 布局内部登记 (仅 JZLayout 调用) ====================

void JZWidget::_layoutAttach(JZLayout* layout)
{
    if (!layout)
        return;
    for (auto* l : m_layoutOwners)
    {
        if (l == layout)
            return;   // 已登记, 防重复
    }
    m_layoutOwners.push_back(layout);
}

void JZWidget::_layoutDetach(JZLayout* layout)
{
    if (!layout)
        return;
    for (auto it = m_layoutOwners.begin(); it != m_layoutOwners.end(); ++it)
    {
        if (*it == layout)
        {
            m_layoutOwners.erase(it);
            return;
        }
    }
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
// 焦点是窗口级概念: 同一原生窗口 (HWND) 内的多个自绘控件共享一个 HWND,
// 故焦点实际指向"当前接收键盘/IME 的控件" (m_focusWidget), 由 JZWindow 持有;
// 控件只负责请求/查询, 真正的进/出焦点事件由 JZWindow::setFocusWidget 派发。

void JZWidget::setFocus()
{
    JZWindow* win = window();
    if (win)
        win->setFocusWidget(this);
}

bool JZWidget::hasFocus() const
{
    JZWindow* win = window();
    return win && win->focusWidget() == this;
}

// ==================== 重绘 ====================
void JZWidget::invalidate(const JZRect &rc)
{
    auto window = this->window();
    if (!window)
        return;

    JZRect global_rc = mapTo(window, rc);
    JZInvalidateEvent* event = new JZInvalidateEvent();
    event->rect = global_rc;
    jzApp->postEvent(window, event);
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

    // 悬停态: 进入/离开控件 (含子树) 由 JZWindow 命中变化派发
    if (e->type() == JZEvent_enter)
    {
        enterEvent(e);
        return true;
    }
    if (e->type() == JZEvent_leave)
    {
        leaveEvent(e);
        return true;
    }

    // 键盘: 由消息层构造 (WM_KEYDOWN -> isPress()==true, WM_KEYUP -> false)
    if (e->type() == JZEvent_key)
    {
        JZKeyEvent* ke = (JZKeyEvent*)e;
        if (ke->isPress())
            keyPressEvent(ke);
        else
            keyReleaseEvent(ke);
        return true;
    }

    // 输入法: 由消息层把 WM_IME_* 翻译后投递给焦点控件
    if (e->type() == JZEvent_ime)
    {
        imeEvent((JZImeEvent*)e);
        return true;
    }

    if (e->type() == JZEvent_focusIn)
    {
        focusInEvent(e);
        return true;
    }

    if (e->type() == JZEvent_focusOut)
    {
        focusOutEvent(e);
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

void JZWidget::enterEvent(JZEvent*)
{
    // 悬浮提示: 设置了 tooltip 文本的控件, 鼠标进入其区域即弹出, 停留期间常显,
    // 移出 (leaveEvent) 立即收起。各业务控件无需各自维护弹层 (框架统一处理)。
    if (!m_tooltipText.isEmpty())
        showTooltip();
}

void JZWidget::leaveEvent(JZEvent*)
{
    // 鼠标离开本控件 (含子树): 立即收起悬浮提示
    hideTooltip();
}

void JZWidget::wheelEvent(JZMouseEvent*)
{
}

void JZWidget::setCursor(const JZCursor& cursor)
{
    if (m_hasCursor && m_cursor == cursor)
        return;
    m_hasCursor = true;
    m_cursor = cursor;
    // 指针正悬停在本控件 (或其子控件) 上时立即刷新系统光标。
    // 无窗口 / 原生窗口未创建时是 no-op (后续 WM_SETCURSOR / 鼠标移动会补上)。
    if (JZWindow* win = window())
        win->applyMouseCursor();
}

void JZWidget::unsetCursor()
{
    if (!m_hasCursor)
        return;
    m_hasCursor = false;
    m_cursor = JZCursor();
    if (JZWindow* win = window())
        win->applyMouseCursor();
}

JZCursor JZWidget::cursor() const
{
    // 自身未显式设置时沿父链继承 (Qt QWidget::cursor 语义), 全无则默认箭头
    for (const JZWidget* w = this; w; w = w->parentWidget())
        if (w->m_hasCursor)
            return w->m_cursor;
    return JZCursor();
}

void JZWidget::grabMouse()
{
    if (JZWindow* win = window())
        win->grabMouse(this);
}

void JZWidget::releaseMouse()
{
    if (JZWindow* win = window())
        win->releaseMouse(this);
}

bool JZWidget::hasMouseGrab() const
{
    const JZWindow* win = window();
    return win && win->mouseGrabber() == this;
}

void JZWidget::keyPressEvent(JZKeyEvent*)
{
}

void JZWidget::keyReleaseEvent(JZKeyEvent*)
{
}

void JZWidget::imeEvent(JZImeEvent*)
{
}

void JZWidget::focusInEvent(JZEvent*)
{
}

void JZWidget::focusOutEvent(JZEvent*)
{
}

void JZWidget::showEvent(JZShowEvent*)
{
}

void JZWidget::hideEvent(JZHideEvent*)
{
    // 控件隐藏 (含所在窗口关闭) 时, 收起其悬浮提示, 避免残留
    hideTooltip();
}

// ==================== 悬浮提示 ====================
void JZWidget::setTooltip(const JZString& tip)
{
    m_tooltipText = tip;
    // 悬停中内容变化 (如 token 用量刷新) 立即刷新弹层; 未悬停则仅记录, 等鼠标移入再弹
    if (m_tooltipPopup && m_tooltipPopup->isVisible())
        updateTooltip();
}

const JZString& JZWidget::tooltip() const
{
    return m_tooltipText;
}

void JZWidget::showTooltip()
{
    if (!m_tooltipPopup) {
        m_tooltipPopup = new JZToolTipWidget(this);
        m_tooltipPopup->setPassThrough(true);   // 纯展示, 不吃鼠标事件
        m_tooltipPopup->setVisible(false);
    }
    updateTooltip();
    m_tooltipPopup->setVisible(true);
}

void JZWidget::hideTooltip()
{
    if (m_tooltipPopup)
        m_tooltipPopup->setVisible(false);
}

void JZWidget::updateTooltip()
{
    JZToolTipWidget* tip = static_cast<JZToolTipWidget*>(m_tooltipPopup);
    tip->setTip(m_tooltipText);

    // 弹层放在控件上方, 右边缘与控件对齐 (窄窗口下左侧不被裁)
    const int w = m_tooltipPopup->width();
    const int h = m_tooltipPopup->height();
    m_tooltipPopup->setGeometry(width() - w, -h - 6, w, h);
}

void JZWidget::resizeEvent(JZResizeEvent*)
{
}

// ==================== 属性 ====================

void JZWidget::setProperty(const std::string& name, const JZVariant& value)
{ return; }

JZVariant JZWidget::property(const std::string& name) const
{ return {}; }

// ==================== AI 语义层 (对齐 Android AccessibilityNodeInfo) ====================

// 默认 role (className 槽位): 类名去 "JZ" 前缀小写 ("JZPushButton" -> "pushbutton")
std::string JZWidget::aiRole() const
{
    std::string cls = className();
    if (cls.size() > 2 && cls.compare(0, 2, "JZ") == 0)
        cls = cls.substr(2);
    for (auto& ch : cls)
        ch = (char)tolower((unsigned char)ch);
    return cls;
}

std::string JZWidget::aiText() const
{
    return {};
}

// contentDesc 槽位: objectName 优先, 否则退化为 role
std::string JZWidget::aiName() const
{
    if (!objectName().empty())
        return objectName();
    return aiRole();
}

bool JZWidget::aiChecked() const
{
    return false;
}

bool JZWidget::aiRangeValue(int& value, int& min, int& max) const
{
    return false;
}

std::vector<std::string> JZWidget::aiItems() const
{
    return {};
}

std::vector<JZAiAction> JZWidget::aiActions() const
{
    return {};
}

// 无障碍树导出: 只含可见控件 (对 AI 是去噪); boundsInScreen 为屏幕逻辑坐标
Json::Value JZWidget::dumpSemanticTree(int maxDepth) const
{
    Json::Value node(Json::objectValue);
    std::string role = aiRole();
    node["className"] = role;
    if (!objectName().empty())
        node["viewId"] = objectName();

    std::string text = aiText();
    if (!text.empty())
        node["text"] = text;
    node["contentDesc"] = aiName();

    // boundsInScreen: 屏幕逻辑坐标 (跨窗口/弹层都能对准像素)
    JZPoint sp = mapToGlobal(JZPoint(0, 0));
    Json::Value bounds(Json::arrayValue);
    bounds.append(sp.x());
    bounds.append(sp.y());
    bounds.append(width());
    bounds.append(height());
    node["boundsInScreen"] = bounds;

    node["visible"] = isVisible();

    // 能力位: 由动作清单与角色推导
    auto acts = aiActions();
    bool clickable = false;
    Json::Value actArr(Json::arrayValue);
    for (const auto& a : acts) {
        if (a.name == "click")
            clickable = true;
        Json::Value o(Json::objectValue);
        o["name"] = a.name;
        if (!a.params.empty())
            o["params"] = a.params;
        actArr.append(o);
    }
    node["clickable"] = clickable;
    if (!actArr.empty())
        node["actions"] = actArr;

    bool checkable = (role == "checkbox" || role == "switch" || role == "radiobutton");
    if (checkable) {
        node["checkable"] = true;
        node["checked"] = aiChecked();
    }
    node["editable"] = (role == "textbox");
    node["scrollable"] = (role == "list" || role == "scrollarea" ||
                          role == "tablewidget" || role == "textedit");

    int rv, rmn, rmx;
    if (aiRangeValue(rv, rmn, rmx)) {
        Json::Value r(Json::objectValue);
        r["value"] = rv;
        r["min"] = rmn;
        r["max"] = rmx;
        node["range"] = r;   // RangeInfo
    }

    auto items = aiItems();
    if (!items.empty()) {
        Json::Value arr(Json::arrayValue);
        for (const auto& s : items)
            arr.append(s);
        node["items"] = arr;   // 框架扩展: owner-draw 列表无法展开成子节点
    }

    if (maxDepth != 0) {
        Json::Value childArr(Json::arrayValue);
        for (auto* obj : children()) {
            auto* c = jzobject_cast<JZWidget*>(obj);
            if (!c || jzobject_cast<JZWindow*>(c))
                continue;                     // 子窗口 (对话框/弹层) 有各自的树
            if (!c->isVisible())
                continue;
            childArr.append(c->dumpSemanticTree(maxDepth - 1));
        }
        if (!childArr.empty())
            node["children"] = childArr;
    }
    return node;
}
