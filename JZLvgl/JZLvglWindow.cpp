#include "JZLvglWindow.h"

JZLvglWindow::JZLvglWindow(JZLvglWidget* parent)
    : JZLvglWidget(lv_win_create, parent)
{
}

JZLvglWindow::JZLvglWindow(JZLvglWidget* parent, int w, int h, const std::string& title)
    : JZLvglWindow(parent)
{
    setSize(w, h);
    lv_win_add_title(m_lv, title.c_str());
}

lv_obj_t* JZLvglWindow::contentLv() const
{
    return lv_win_get_content(m_lv);
}

lv_obj_t* JZLvglWindow::headerLv() const
{
    return lv_win_get_header(m_lv);
}

void JZLvglWindow::addTitleBtn(const void* icon, int32_t btnW)
{
    lv_win_add_button(m_lv, icon, btnW);
}
