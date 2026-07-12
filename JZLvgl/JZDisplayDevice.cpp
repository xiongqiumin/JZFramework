#include "JZDisplayDevice.h"
#include "JZStd.h"
#include "lvgl.h"

#if JZ_OS == JZ_OS_WINDOWS
#include "src/drivers/windows/lv_windows_display.h"
#include "src/drivers/windows/lv_windows_input.h"
#endif

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
