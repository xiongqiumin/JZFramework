#include "JZMainWindow.h"

JZ_OBJECT_IMPL(JZMainWindow, JZWidget)

JZMainWindow::JZMainWindow(JZWidget* parent)
    : JZWidget(lv_win_create, parent)
{
}

JZMainWindow::JZMainWindow(JZWidget* parent, int w, int h, const std::string& title)
    : JZMainWindow(parent)
{
    setSize(w, h);
    lv_win_add_title(m_lv, title.c_str());
}

void JZMainWindow::addTitleBtn(const void* icon, int32_t btnW)
{
    lv_win_add_button(m_lv, icon, btnW);
}
