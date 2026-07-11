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
    lvgl_init();
#endif
#endif
}


void JZDriver::deinit()
{
#ifdef HAS_JZLvgl
#ifdef _WIN32
    lvgl_deinit();
#endif
#endif
}

#endif
