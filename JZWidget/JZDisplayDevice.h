#ifndef JZ_DISPLAY_DEVICE_H_
#define JZ_DISPLAY_DEVICE_H_

#include "JZWidget.h"

class JZDisplayDevice : public JZWidget
{
public:
    JZDisplayDevice(void* display);
    JZDisplayDevice(int w,int h);    
    ~JZDisplayDevice();

    void* displayHandle() const;

private:
    void* m_display = nullptr;
};

#endif
