#ifndef JZ_SCREEN_H_
#define JZ_SCREEN_H_

#include "JZLvglWidget.h"

class JZScreen : public JZLvglWidget
{
public:
    JZScreen(lv_obj_t *screen);
    ~JZScreen();
};

#endif
