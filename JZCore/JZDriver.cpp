#include "JZPlatform.h"
#include "JZDriver.h"

// === 头文件 ===
#ifdef HAS_JZLvgl
#include "lvgl.h"
#endif

#ifdef ESP_PLATFORM

#include "JZPlatformWrapperEsp32.h"

#ifdef HAS_JZBle
#include "JZBleDriverEsp32.h"
#endif
#ifdef HAS_JZWifi
#include "JZWifiDriverEsp32.h"
#endif
#ifdef HAS_JZGpio
#include "JZGpioDriverEsp32.h"
#endif

// ==================== init ====================

void JZDriver::init()
{
    JZPlatform::setInstance(new JZPlatformWrapperEsp32());

#ifdef HAS_JZBle
    JZBleDriver::setInstance(new JZBleDriverEsp32());
#endif

#ifdef HAS_JZWifi
    JZWifiDriver::setInstance(new JZWifiDriverEsp32());
#endif

#ifdef HAS_JZGpio
    JZGpioDriver::addInstance(new JZGpioDriverEsp32());
#endif
}


#else

#ifdef HAS_JZSimulator
#ifdef HAS_JZBle
#include "JZBleDriverSimulator.h"
#endif

#ifdef HAS_JZGpio
#include "JZGpioDriverSimulator.h"
#endif
#endif // HAS_JZSimulator


void JZDriver::init()
{
#ifdef HAS_JZLvgl
#ifdef _WIN32
    lv_init();    
    
    int w = 800, h = 600;
    auto display = lv_windows_create_display(L"", w, h, 100, false, false);
    lv_windows_acquire_pointer_indev(display);
    lv_windows_acquire_keypad_indev(display);
    lv_windows_acquire_encoder_indev(display);

    // 让 lv_windows_display_timer_callback 尽早执行，初始化 draw buffer
    lv_timer_handler();
#endif
#endif
}

#endif

// ==================== deinit ====================

void JZDriver::deinit()
{
#ifdef HAS_JZBle
    JZBleDriver::setInstance(nullptr);
#endif

#ifdef HAS_JZWifi
    JZWifiDriver::setInstance(nullptr);
#endif

#ifdef HAS_JZGpio
    JZGpioDriver::clearInstances();
#endif

    JZPlatform::setInstance(nullptr);
}