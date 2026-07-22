#include "JZDisplayDevice.h"
#include "JZStd.h"
#include "lvgl.h"

JZDisplayDevice::JZDisplayDevice(lv_display_t* display)
    :JZWidget(NoCreate())
{
    m_display = display;
    m_lv = lv_display_get_screen_active(display);
}

JZDisplayDevice::JZDisplayDevice(int w,int h)
    :JZWidget(NoCreate())
{
#ifdef _WIN32
    m_display = lv_windows_create_display(L"", w, h, 100, false, false);
    m_lv = lv_display_get_screen_active(m_display);
    
    lv_windows_acquire_pointer_indev(m_display);
    lv_windows_acquire_keypad_indev(m_display);
    lv_windows_acquire_encoder_indev(m_display);
#endif
}

JZDisplayDevice::~JZDisplayDevice()
{    
#ifdef _WIN32

#endif
}

lv_display_t* JZDisplayDevice::displayHandle() const
{
    return m_display;
}
