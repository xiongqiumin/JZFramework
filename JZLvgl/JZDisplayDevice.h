#ifndef JZ_DISPLAY_DEVICE_H_
#define JZ_DISPLAY_DEVICE_H_

#include "JZWidget.h"

class JZDisplayDevice
{
public:
    JZDisplayDevice(int w, int h);
    ~JZDisplayDevice();

    lv_display_t* displayHandle() const;

private:
    lv_display_t* m_display;
};

#endif
