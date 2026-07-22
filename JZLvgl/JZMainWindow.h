#ifndef JZ_LVGL_WINDOW_H_
#define JZ_LVGL_WINDOW_H_

#include "JZDisplayDevice.h"

class JZMainWindow : public JZWidget
{
    JZ_OBJECT_DECLARE(JZMainWindow)

public:
    explicit JZMainWindow(JZDisplayDevice *device);
};

#endif
