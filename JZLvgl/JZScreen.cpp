#include "JZScreen.h"
#include "JZApplication.h"

JZ_OBJECT_IMPL(JZScreen, JZWidget)

JZScreen::JZScreen(lv_obj_t *screen)
    : JZWidget(NoCreate{}, nullptr)
{
    setLvglObject(screen);
}

JZScreen::~JZScreen()
{
}