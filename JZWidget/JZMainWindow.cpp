#include "JZMainWindow.h"
#include "JZAction.h"
#include "JZColor.h"
#include "JZLayout.h"
#include "JZMenu.h"
#include "JZMenuBar.h"
#include "JZPainter.h"

JZ_OBJECT_IMPL(JZMainWindow, JZWindow)

JZMainWindow::JZMainWindow(JZWidget* parent)
    : JZWindow(parent)
{
    // 内部骨架: 无内边距垂直排列 菜单栏(第0项, stretch 0) -> 中央区(stretch 1)。
    // 子控件在 addMenuBar / setCentralWidget 时才加入, 所以这里只建空布局。
    setLayout(new JZVBoxLayout(0, 0));
}

void JZMainWindow::addMenuBar(JZMenuBar* menuBar)
{
    if (!menuBar || menuBar == m_menuBar)
        return;
    JZBoxLayout* box = static_cast<JZBoxLayout*>(layout());
    if (m_menuBar)
        box->removeWidget(m_menuBar);   // 替换旧的, 不删除对象
    m_menuBar = menuBar;
    menuBar->setParentWidget(this);
    box->insertWidget(0, menuBar, 0);   // 菜单栏固定插到第 0 项, 永远置顶
}

void JZMainWindow::setCentralWidget(JZWidget* widget)
{
    if (!widget || widget == m_centralWidget)
        return;
    JZBoxLayout* box = static_cast<JZBoxLayout*>(layout());
    if (m_centralWidget)
        box->removeWidget(m_centralWidget);   // 替换旧的, 不删除对象
    m_centralWidget = widget;
    widget->setParentWidget(this);
    box->addWidget(widget, 1);                // 剩余空间全给中央区
}

void JZMainWindow::paintEvent(JZPainterEvent* event)
{
    // ⚠ 底色别在这里写死: 窗口默认色(浅灰 #F0F0F0, 见 JZWindow.cpp 的 kDefaultWindowBackground)
    // 与样式表的解析都在 JZWindow::paintEvent 里, 这里曾经无条件填白, 把默认色盖回去了。
    JZWindow::paintEvent(event);
}

// ---------------------------------------------------------------------------
// 菜单栏里的菜单项不在控件父子链里, 快捷键分发拿不到 —— 把配了 shortcut 的交出去
// (见 JZWindow::dispatchShortcut; 每次分发时现收集, 所以挂栏之后再 setShortcut 也有效)
// ---------------------------------------------------------------------------
namespace {

void collectMenuActions(JZMenu* menu, std::vector<JZObjectPointer>& out)
{
    if (!menu)
        return;

    const std::vector<JZAction*>& acts = menu->actions();
    for (size_t i = 0; i < acts.size(); ++i)
    {
        JZAction* a = acts[i];
        if (!a)
            continue;

        if (!a->shortcut().empty())
            out.push_back(a->guardPointer());   // 只交配了快捷键的: 其余不参与按键匹配

        if (JZMenu* sub = a->subMenu())         // 子菜单递归
            collectMenuActions(sub, out);
    }
}

} // namespace

std::vector<JZObjectPointer> JZMainWindow::extraActions() const
{
    std::vector<JZObjectPointer> out;
    if (!m_menuBar)
        return out;

    for (int i = 0; i < m_menuBar->menuCount(); ++i)
        collectMenuActions(m_menuBar->menuAt(i), out);

    return out;
}
