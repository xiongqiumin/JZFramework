#ifndef JZ_DISPLAY_DEVICE_H_
#define JZ_DISPLAY_DEVICE_H_

#include "JZWidget.h"

class JZDisplayDevice : public JZWidget
{
public:
    JZDisplayDevice(lv_display_t* m_display);    
    ~JZDisplayDevice();

    lv_display_t* displayHandle() const;

private:
    lv_display_t* m_display;
};

#endif
