#include "JZMenu.h"
#include "JZAction.h"
#include "JZWindow.h"
#include "JZPainter.h"
#include "JZFont.h"
#include "JZFontMetrics.h"
#include "JZPen.h"
#include "JZBrush.h"
#include "JZColor.h"

#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// ==================== 内部: 菜单内容绘制控件 ====================
// JZMenuView 是弹层 (JZWindow::Popup) 内的唯一子控件, 负责把 JZMenu 的
// 条目 (JZAction) 自绘出来, 并处理鼠标/键盘交互。它不持有数据, 只引用 JZMenu。
class JZMenuView : public JZWidget
{
    JZ_OBJECT_DECLARE(JZMenuView)
public:
    explicit JZMenuView(JZWidget* parent = nullptr)
        : JZWidget(parent) {}

    void setMenu(JZMenu* m) { m_menu = m; }
    JZMenu* menu() const { return m_menu; }

    void setOwner(JZWindow* w) { m_owner = w; }

protected:
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseMoveEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;
    void keyPressEvent(JZKeyEvent* event) override;

private:
    void openSubMenu(int index);
    void closeSubMenu();
    void stepHover(int dir);

    JZMenu* m_menu = nullptr;
    JZWindow* m_owner = nullptr;
    JZMenu* m_submenu = nullptr;   // 当前打开的子菜单
};

JZ_OBJECT_IMPL(JZMenuView, JZWidget)

// 布局常量
static const int kMenuFont = 13;
static const int kItemH = 28;
static const int kSepH = 9;
static const int kPadX = 6;
static const int kIconW = 20;      // 勾选/图标列宽
static const int kTextX = kPadX + kIconW;
static const int kRightPad = 12;
static const int kShortcutGap = 18;
static const int kArrowW = 12;
static const int kMinWidth = 120;

void JZMenuView::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width(), h = height();

    p.fillRect(0, 0, w, h, JZColor(0xfa, 0xfa, 0xfa));
    p.setPen(JZPen(JZColor(0xb4, 0xb4, 0xb4), 1));
    p.drawRect(0, 0, w, h);

    JZFont font(kMenuFont);
    for (int i = 0; i < m_menu->itemCount(); ++i) {
        JZAction* a = m_menu->actionAt(i);
        JZRect r = m_menu->itemRect(i);

        if (!a->isVisible())
            continue;

        if (a->isSeparator()) {
            p.setPen(JZPen(JZColor(0xd8, 0xd8, 0xd8), 1));
            int y = r.top() + r.height() / 2;
            p.drawLine(kPadX, y, w - kPadX, y);
            continue;
        }

        bool hovered = (i == m_menu->hoverIndex()) && a->isEnabled();
        if (hovered)
            p.fillRect(r.left(), r.top(), r.width(), r.height(), JZColor(0x2d, 0x6c, 0xdf));

        JZColor textCol = a->isEnabled()
            ? (hovered ? JZColor(0xff, 0xff, 0xff) : JZColor(0x20, 0x20, 0x20))
            : JZColor(0xa6, 0xa6, 0xa6);
        p.setPen(JZPen(textCol, 1));
        p.setFont(font);

        // 勾选标记
        if (a->isCheckable() && a->isChecked()) {
            int cx = kPadX + 4, cy = r.top() + r.height() / 2;
            p.drawLine(cx, cy, cx + 4, cy + 4);
            p.drawLine(cx + 4, cy + 4, cx + 9, cy - 5);
        }

        // 文本
        int ty = JZFontMetrics::centerY(font, r.height()) + r.top();
        p.drawText(kTextX, ty, a->text().c_str());

        // 快捷键 (右对齐)
        if (!a->shortcut().empty()) {
            int sw = JZFontMetrics::textWidth(a->shortcut(), font);
            int sx = w - kRightPad - sw - (a->subMenu() ? kArrowW : 0);
            p.drawText(sx, ty, a->shortcut().c_str());
        }

        // 子菜单箭头
        if (a->subMenu()) {
            int ax = w - kRightPad - 2;
            int ay = r.top() + r.height() / 2;
            p.drawLine(ax, ay - 4, ax + 5, ay);
            p.drawLine(ax + 5, ay, ax, ay + 4);
        }
    }
}

void JZMenuView::mousePressEvent(JZMouseEvent* event)
{
    // 按下即展开/收起子菜单; 叶子项在 release 时触发
    int idx = m_menu->hitTestItem(event->localPos().y());
    if (idx < 0)
        return;
    JZAction* a = m_menu->actionAt(idx);
    if (a->isEnabled() && a->subMenu())
        openSubMenu(idx);
}

void JZMenuView::mouseMoveEvent(JZMouseEvent* event)
{
    int idx = m_menu->hitTestItem(event->localPos().y());
    if (idx != m_menu->hoverIndex()) {
        m_menu->setHoverIndex(idx);
        update();
    }
    if (idx < 0) {
        closeSubMenu();
        return;
    }
    JZAction* a = m_menu->actionAt(idx);
    if (a->isEnabled() && a->subMenu()) {
        if (m_submenu != a->subMenu()) {
            openSubMenu(idx);
            // popup() 是阻塞的: 返回时状态可能已被更新一帧的 move 事件改写,
            // 本帧(事件已过期)到此为止, 下一帧 move 会按当前状态重新判定。
            return;
        }
    } else {
        closeSubMenu();
    }
}

void JZMenuView::mouseReleaseEvent(JZMouseEvent* event)
{
    int idx = m_menu->hitTestItem(event->localPos().y());
    if (idx < 0)
        return;
    JZAction* a = m_menu->actionAt(idx);
    if (!a->isEnabled() || a->isSeparator())
        return;
    if (a->subMenu()) {        // 点子菜单头: 展开, 不触发
        openSubMenu(idx);
        return;
    }
    m_menu->activateAction(idx);
}

void JZMenuView::keyPressEvent(JZKeyEvent* event)
{
    if (event->key() == JZKey::Key_Down) {
        stepHover(1); event->accept();
    } else if (event->key() == JZKey::Key_Up) {
        stepHover(-1); event->accept();
    } else if (event->key() == JZKey::Key_Right) {
        int cur = m_menu->hoverIndex();
        if (cur >= 0) {
            JZAction* a = m_menu->actionAt(cur);
            if (a->isEnabled() && a->subMenu())
                openSubMenu(cur);
        }
        event->accept();
    } else if (event->key() == JZKey::Key_Left) {
        if (m_menu->parentMenu())
            m_menu->hidePopup();
        event->accept();
    } else if (event->key() == JZKey::Key_Return || event->key() == JZKey::Key_Space) {
        int cur = m_menu->hoverIndex();
        if (cur >= 0) {
            JZAction* a = m_menu->actionAt(cur);
            if (a->isEnabled() && a->subMenu())
                openSubMenu(cur);
            else
                m_menu->activateAction(cur);
        }
        event->accept();
    }
}

void JZMenuView::stepHover(int dir)
{
    int n = m_menu->itemCount();
    if (n == 0)
        return;
    int cur = m_menu->hoverIndex();
    int i = (cur < 0) ? 0 : cur;
    for (int step = 0; step < n; ++step) {
        i = (i + dir + n) % n;
        if (i == cur)
            break;
        JZAction* a = m_menu->actionAt(i);
        if (!a->isSeparator() && a->isEnabled() && a->isVisible())
            break;
    }
    if (i != cur) {
        m_menu->setHoverIndex(i);
        update();
    }
}

void JZMenuView::openSubMenu(int index)
{
    closeSubMenu();
    JZAction* a = m_menu->actionAt(index);
    JZMenu* sub = a ? a->subMenu() : nullptr;
    if (!sub)
        return;
    if (sub->isPopupOpen())
        return;   // 重入保护: 上一次 popup() 的模态循环还没返回, 此时再 ensurePopup
                  // 会 delete 掉正在跑模态循环的弹层窗口 (悬空指针)

    sub->setParentMenu(m_menu);
    JZRect ir = m_menu->itemRect(index);
    JZPoint gp = this->mapToGlobal(ir.topLeft());
    // 锚点 = 该菜单项在屏幕上的矩形 (x, y, w, h); 子菜单由 popup 自行向右侧展开
    JZRect anchor(gp.x(), gp.y(), ir.width(), ir.height());
    // 先登记再进入模态: popup() 阻塞期间父菜单仍会收到鼠标事件(已不再抓取),
    // 后续 move 靠 m_submenu 判断"当前该关谁"。
    m_submenu = sub;
    sub->popup(m_owner, anchor, true);   // 嵌套模态: 阻塞直到子菜单关闭
}

void JZMenuView::closeSubMenu()
{
    if (m_submenu) {
        m_submenu->hidePopup();
        m_submenu = nullptr;
    }
}

// ==================== JZMenu ====================
JZ_OBJECT_IMPL(JZMenu, JZObject)

JZMenu::JZMenu(JZObject* parent)
    : JZObject(parent)
    , aboutToShow(this)
    , aboutToHide(this)
    , sigActionTriggered(this)
{
}

JZMenu::JZMenu(const std::string& title, JZObject* parent)
    : JZMenu(parent)
{
    m_title = title;
}

JZMenu::~JZMenu()
{
    if (m_popup)
        delete m_popup;   // 顺带删除内部 JZMenuView
}

void JZMenu::setTitle(const std::string& title)
{
    m_title = title;
}

std::string JZMenu::title() const
{
    return m_title;
}

void JZMenu::setIcon(const JZImage& icon)
{
    m_icon = icon;
}

const JZImage& JZMenu::icon() const
{
    return m_icon;
}

JZAction* JZMenu::addAction(const std::string& text)
{
    JZAction* a = new JZAction(text, this);
    m_actions.push_back(a);
    connectActionIndex(a);
    return a;
}

// 把"某项被触发"转成 sigActionTriggered(索引): JS 侧按索引认项(值级, 不持 action 句柄)。
// 索引在**触发时**现算, 所以之后再有增删也认得对(本类的菜单项只增不删, 见 actionAt)。
void JZMenu::connectActionIndex(JZAction* action)
{
    if (!action)
        return;

    JZMenu* self = this;
    connect(&action->triggered, [self, action]() {
        for (size_t i = 0; i < self->m_actions.size(); i++)
        {
            if (self->m_actions[i] == action)
            {
                self->sigActionTriggered.emit((int)i);
                return;
            }
        }
    });
}

JZAction* JZMenu::addAction(const std::string& text, std::function<void()> triggered)
{
    JZAction* a = addAction(text);
    if (triggered)
        connect(&a->triggered, [triggered]() { triggered(); });
    return a;
}

JZAction* JZMenu::addAction(JZAction* action)
{
    if (!action)
        return nullptr;
    action->setParent(this);
    m_actions.push_back(action);
    connectActionIndex(action);
    return action;
}

void JZMenu::addSeparator()
{
    JZAction* a = new JZAction(this);
    a->setSeparator(true);
    m_actions.push_back(a);
}

JZMenu* JZMenu::addMenu(const std::string& title)
{
    JZMenu* sub = new JZMenu(title, this);
    // 父子关系是**建的时候就有**的结构关系, 这里就定下来。
    // 以前只在 JZMenuView::openSubMenu 里设(即"打开过才有爹"), 于是任何"没等打开就走父链"的
    // 逻辑都会静默失效: closeAll() 收不起整条弹层链(弹层关不掉, 模态循环卡死)、
    // exec() 沿父链写回结果找不到发起者。openSubMenu 里那次保留(它兼管 addAction(JZAction*)
    // 把子菜单挪到别的父菜单下的情形)。
    sub->setParentMenu(this);
    JZAction* a = new JZAction(title, this);
    a->setSubMenu(sub);
    m_actions.push_back(a);
    return sub;
}

void JZMenu::clear()
{
    for (auto* a : m_actions)
        delete a;
    m_actions.clear();
    m_itemRects.clear();
}

int JZMenu::actionCount() const
{
    return (int)m_actions.size();
}

JZAction* JZMenu::actionAt(int index) const
{
    if (index >= 0 && index < (int)m_actions.size())
        return m_actions[index];
    return nullptr;
}

int JZMenu::menuCount() const
{
    int n = 0;
    for (auto* a : m_actions)
        if (a->subMenu())
            n++;
    return n;
}

JZMenu* JZMenu::menuAt(int index) const
{
    if (index < 0)
        return nullptr;
    int n = 0;
    for (auto* a : m_actions) {
        if (a->subMenu()) {
            if (n == index)
                return a->subMenu();
            n++;
        }
    }
    return nullptr;
}

void JZMenu::updateLayout()
{
    JZFont font(kMenuFont);
    int w = 0;
    int y = 2;
    m_itemRects.clear();

    for (auto* a : m_actions) {
        // 不可见项: 仍占一个 rect 位(与 m_actions 一一对应 ⇒ 下标不移动), 但高度 0 ——
        // 于是不绘制、命中不到(hitTestItem 判 y < top-1)、键盘巡航也会跳过(见 stepHover)。
        if (!a->isVisible()) {
            m_itemRects.push_back(JZRect(0, y, 0, 0));
            continue;
        }
        if (a->isSeparator()) {
            m_itemRects.push_back(JZRect(0, y, 10, kSepH));
            y += kSepH;
            continue;
        }
        int total = kTextX + JZFontMetrics::textWidth(a->text(), font) + kRightPad;
        if (!a->shortcut().empty())
            total += JZFontMetrics::textWidth(a->shortcut(), font) + kShortcutGap;
        if (a->subMenu())
            total += kArrowW;
        w = std::max(w, total);
        m_itemRects.push_back(JZRect(0, y, 10, kItemH));
        y += kItemH;
    }
    y += 2;
    if (w < kMinWidth)
        w = kMinWidth;
    m_contentSize = JZSize(w, y);
    for (auto& r : m_itemRects)
        r.setWidth(w);
}

int JZMenu::hitTestItem(int y) const
{
    for (int i = 0; i < (int)m_itemRects.size(); ++i) {
        if (y >= m_itemRects[i].top() && y < m_itemRects[i].bottom())
            return i;
    }
    return -1;
}

void JZMenu::activateAction(int index)
{
    if (index < 0 || index >= (int)m_actions.size())
        return;
    JZAction* a = m_actions[index];
    if (!a || !a->isEnabled() || a->isSeparator())
        return;
    if (a->isCheckable())
        a->setChecked(!a->isChecked());

    // 先收起整条弹层链, 再执行动作 (Qt 语义: 菜单关闭后动作才生效)。
    // 反过来 (先 emit 后收起) 时, 动作处理器里再开的模态对话框出现时弹层还开着,
    // 用户点对话框的关闭按钮会被弹层的"外部按下关闭"吞掉, 表现为要点两次才关。
    closeAll();

    // 同步 exec() 的结果: 沿父链找**离本项最近的、正在 exec 的菜单**写回。
    // 为什么要沿父链: 子菜单里的项走的是子菜单自己的 activateAction, 而发起弹层/调用 exec 的
    // 是父菜单(甚至更上层) —— 结果得写到那次 exec 的接收者身上, 弹层返回时它才读得到。
    // 就近(break)是给"菜单项的处理器里又 exec 一个菜单"这种嵌套留的: 内层不污染外层现场。
    for (JZMenu* m = this; m; m = m->m_parentMenu)
    {
        if (m->m_execMode)
        {
            m->m_execResult = a;
            break;
        }
    }

    a->triggered.emit();
}

JZAction* JZMenu::exec(JZWindow* owner, const JZRect& anchor)
{
    if (m_actions.empty())
        return nullptr;          // 空菜单: popup() 也会早退, 直接给 nullptr(不阻塞)

    // 保存/恢复现场: exec 可重入(处理器里再 exec 一个菜单), 逐层各记各的结果。
    JZAction* prevResult = m_execResult;
    const bool prevMode = m_execMode;
    m_execResult = nullptr;
    m_execMode = true;

    popup(owner, anchor);        // 阻塞到菜单关闭(内部 execModal)

    JZAction* result = m_execResult;
    m_execResult = prevResult;
    m_execMode = prevMode;
    return result;
}

void JZMenu::closeAll()
{
    if (m_parentMenu)
        m_parentMenu->closeAll();
    hidePopup();
}

void JZMenu::ensurePopup()
{
    if (m_popup) {
        delete m_popup;     // 同时删除内部 JZMenuView
        m_popup = nullptr;
        m_view = nullptr;
    }
    m_popup = new JZWindow(nullptr);
    m_popup->setWindowType(JZWindow::Popup);
    m_popup->setAutoPosition(false);
    m_view = new JZMenuView(m_popup);
    m_view->setMenu(this);
}

void JZMenu::popup(JZWindow* owner, const JZRect& anchor, bool asSubmenu)
{
    if (m_actions.empty())
        return;

    ensurePopup();
    // aboutToShow 必须早于 updateLayout: 接收方通常在回调里增删菜单项（如"最近的项目"），
    // 若先算布局，回调改完内容后 m_itemRects / m_contentSize 仍是旧的，弹层尺寸与命中都会错位。
    aboutToShow.emit();
    updateLayout();

    // 浮层跟随 owner 的 DPI 缩放: 否则 createWindow 会用主屏 DPI, 与 mapToGlobal
    // (owner 缩放) 算出的 anchor 不在同一坐标系, 多屏/HiDPI 下弹层错位、尺寸不对。
    if (owner && owner->device())
        m_popup->setDeviceScale(owner->device()->scale());

    int cw = m_contentSize.width();
    int ch = m_contentSize.height();
    m_view->setOwner(owner);
    m_view->setGeometry(0, 0, cw, ch);
    m_popup->resize(cw, ch);

    float s = (owner && owner->device()) ? owner->device()->scale() : 1.0f;
    int screenW = (int)(GetSystemMetrics(SM_CXSCREEN) / s);
    int screenH = (int)(GetSystemMetrics(SM_CYSCREEN) / s);

    // 由锚点矩形推导落点 (一律用 left/top + width/height, 不用 right()/bottom():
    // JZRect 是 (x,y,w,h) 且 right/bottom 含 -1 的闭区间语义, 直接拿来做外边缘会偏)
    int anchorL = anchor.left();
    int anchorT = anchor.top();
    int anchorR = anchor.left() + anchor.width();
    int anchorB = anchor.top() + anchor.height();

    JZPoint pos = asSubmenu
        ? JZPoint(anchorR, anchorT)      // 子菜单: 贴锚点右侧
        : JZPoint(anchorL, anchorB);     // 下拉: 贴锚点下沿

    if (asSubmenu) {
        if (pos.x() + cw > screenW)
            pos.setX(std::max(0, anchorL - cw));          // 向左翻 (贴父菜单左沿)
        if (pos.y() + ch > screenH)
            pos.setY(std::max(0, screenH - ch));          // 上移贴屏幕底
    } else {
        if (pos.x() + cw > screenW)
            pos.setX(std::max(0, screenW - cw));
        if (pos.y() + ch > screenH)
            pos.setY(std::max(0, anchorT - ch));          // 向上翻 (贴锚点上沿)
    }

    m_popup->moveScreen(pos.x(), pos.y());
    m_popup->show();
    m_view->setFocus();
    m_popup->execModal(false);     // 不禁用 owner; 外点/Esc 由弹层抓取关闭

    aboutToHide.emit();
    m_hover = -1;
}

void JZMenu::hidePopup()
{
    if (isPopupOpen())
        m_popup->hide();
}

bool JZMenu::isPopupOpen() const
{
    return m_popup && m_popup->isVisible();
}

std::vector<std::string> JZMenu::aiItems() const
{
    std::vector<std::string> v;
    for (auto* a : m_actions) {
        if (!a->isSeparator() && a->isVisible())
            v.push_back(a->text());
    }
    return v;
}
