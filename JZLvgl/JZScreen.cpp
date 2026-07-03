#include "JZScreen.h"
#include "JZApplication.h"

static lv_obj_t* s_screenCreate(lv_obj_t*) { return lv_screen_active(); }

JZScreen* JZScreen::active()
{
    static JZScreen *s = new JZScreen();
    s->setParent(jzApp);
    return s;
}

JZScreen::JZScreen()
    : JZLvglWidget(s_screenCreate, nullptr)
{
}
