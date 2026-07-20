#include "JZDisplayDevice.h"
#include "JZStd.h"
#include "lvgl.h"

JZDisplayDevice::JZDisplayDevice(lv_display_t* display)
    :JZWidget(NoCreate(), nullptr)
{
    m_display = display;
    m_lv = lv_display_get_screen_active(display);
}


JZDisplayDevice::~JZDisplayDevice()
{    
}

lv_display_t* JZDisplayDevice::displayHandle() const
{
    return m_display;
}
