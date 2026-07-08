#include "JZMainWindow.h"

JZ_OBJECT_IMPL(JZMainWindow, JZWidget)

JZMainWindow::JZMainWindow(JZWidget* parent)
    : JZWidget(lv_win_create, parent)
{
}

const std::string& JZMainWindow::title() const
{
    return lv_win_title(m_lv);
}

void JZMainWindow::setTitle(const std::string& title)
{
    resize(w, h);
    lv_win_add_title(m_lv, title.c_str());
}