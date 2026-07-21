#include "JZDisplayDevice.h"
#include "JZStd.h"
#include "lvgl.h"

static int g_lvgl_device = 0;
JZDisplayDevice::JZDisplayDevice(int w,int h)
{
#ifdef _WIN32
    if(g_lvgl_device == 0)
    {
        lv_init();    
    }
    g_lvgl_device++;

    m_display = lv_windows_create_display(L"", w, h, 100, false, false);
    lv_windows_acquire_pointer_indev(m_display);
    lv_windows_acquire_keypad_indev(m_display);
    lv_windows_acquire_encoder_indev(m_display);
#endif
}


JZDisplayDevice::~JZDisplayDevice()
{    
#ifdef _WIN32
    g_lvgl_device--;
    if(g_lvgl_device == 0)
    {
        lv_deinit();
    }
#endif
}

lv_display_t* JZDisplayDevice::displayHandle() const
{
    return m_display;
}
