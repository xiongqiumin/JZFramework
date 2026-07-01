#ifndef JZ_SCREEN_H_
#define JZ_SCREEN_H_

#include "JZLvglWidget.h"

class JZScreen : public JZLvglWidget
{
public:
    static JZScreen* active();

private:
    JZScreen();
};

#endif
