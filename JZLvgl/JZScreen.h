#ifndef JZ_SCREEN_H_
#define JZ_SCREEN_H_

#include "JZWidget.h"

class JZScreen : public JZWidget
{
    JZ_OBJECT_DECLARE(JZScreen)

public:
    JZScreen(lv_obj_t *screen);
    ~JZScreen();
};

#endif
