#include "JZScreen.h"
#include "JZApplication.h"

JZScreen::JZScreen(lv_obj_t *screen)
    : JZLvglWidget(NoCreate{}, nullptr)
{
    setLvglObject(screen);
}

JZScreen::~JZScreen()
{
}