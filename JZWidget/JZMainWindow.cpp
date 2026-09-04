#include "JZMainWindow.h"
#include "JZColor.h"
#include "JZLayout.h"
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
    JZPainter p(this);
    p.fillRect(rect(), JZColor::White);
}
