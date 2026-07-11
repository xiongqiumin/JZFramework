#include "JZDisplayDevice.h"
#include "JZStd.h"

#if JZ_OS == JZ_OS_WINDOWS
#include "lvgl/drivers/windows/lv_windows_display.h"
#include "lvgl/drivers/windows/lv_windows_input.h"
#endif

JZDisplayDevice::JZDisplayDevice(int w, int h)
{
#if JZ_OS == JZ_OS_WINDOWS
    m_display = lv_windows_create_display(L"", w, h, 100, false, false);
    lv_windows_acquire_pointer_indev(m_display);
    lv_windows_acquire_keypad_indev(m_display);
    lv_windows_acquire_encoder_indev(m_display);
#endif
}


JZDisplayDevice::~JZDisplayDevice()
{
    if (m_display)
        lv_display_delete(m_display);
}

lv_display_t* JZDisplayDevice::displayHandle() const
{
    return m_display;
}
