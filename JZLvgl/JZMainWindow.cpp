#include "JZMainWindow.h"

JZ_OBJECT_IMPL(JZMainWindow, JZWidget)

JZMainWindow::JZMainWindow(JZWidget* parent)
    : JZWidget(lv_win_create, parent)
{
}

JZMainWindow::JZMainWindow(JZWidget* parent, int w, int h, const std::string& title)
    : JZMainWindow(parent)
{
    resize(w, h);
    setTitle(title);
}

std::string JZMainWindow::title() const
{
    return m_title;
}

void JZMainWindow::setTitle(const std::string& title)
{
    m_title = title;
    lv_win_add_title(m_lv, title.c_str());
}

lv_obj_t* JZMainWindow::addButton(const void* icon, int32_t btn_w)
{
    return lv_win_add_button(m_lv, icon, btn_w);
}
