#ifndef JZ_SCREEN_H_
#define JZ_SCREEN_H_

#include "JZLvglWidget.h"

class JZScreen : public JZLvglWidget
{
public:
    enum ScreenType{
        ActiveScreen,
        CreateNewScreen,
    };

    JZScreen(ScreenType type = ActiveScreen);
    ~JZScreen();
};

#endif
