#include "JZScreen.h"
#include "JZApplication.h"

static lv_obj_t* s_screenCreate(lv_obj_t*) { return lv_screen_active(); }

JZScreen::JZScreen(ScreenType type)
    : JZLvglWidget(s_screenCreate, nullptr)
{
}

JZScreen::~JZScreen()
{
}