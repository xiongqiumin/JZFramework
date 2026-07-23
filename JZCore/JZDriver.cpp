#include "JZPlatform.h"
#include "JZDriver.h"

// === 头文件 ===
#ifdef HAS_JZLvgl
#include "lvgl.h"
#endif

#ifdef HAS_JZBle
#include "JZBleDriver.h"
#endif
#ifdef HAS_JZWifi
#include "JZWifiDriver.h"
#endif
#ifdef HAS_JZGpio
#include "JZGpioDriver.h"
#endif
#ifdef HAS_JZSensor
#include "JZSensorDriver.h"
#endif
#ifdef HAS_JZSerialPort
#include "JZSerialPortDriver.h"
#endif

#ifdef HAS_JZOTA
#include "JZOTADriver.h"
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
#ifdef HAS_JZSensor
#include "JZSensorDriverEsp32.h"
#endif
#ifdef HAS_JZSerialPort
#include "JZSerialDriverEsp32.h"
#endif

#ifdef HAS_JZOTA
#include "JZOTADriverEsp32.h"
#endif

#ifdef HAS_JZNet
#include "JZNet/JZNetPacket.h"
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

#ifdef HAS_JZSensor
    JZSensorDriver::addInstance(new JZSensorDriverEsp32());
#endif

#ifdef HAS_JZSerialPort
    JZSerialPortDriver::setInstance(new JZSerialDriverEsp32());
#endif

#ifdef HAS_JZOTA
    JZOTADriver::setInstance(new JZOTADriverEsp32());
#endif

#ifdef HAS_JZNet
    JZNetPackInit();
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

#ifdef HAS_JZSerialPort
#if defined(_WIN32)
#include "JZSerialDriverWindows.h"
#elif defined(__linux__)
#include "JZSerialDriverLinux.h"
#endif
#endif // HAS_JZSerialPort

#ifdef HAS_JZAudio
#include "JZAudio/JZAudioDriver.h"
#if defined(_WIN32)
#include "JZAudio/detail/windows/JZAudioDriverWindows.h"
#elif defined(__linux__)
#include "JZAudio/detail/linux/JZAudioDriverLinux.h"
#endif
#endif // HAS_JZAudio

#ifdef HAS_JZNet
#include "JZNet/JZNetPacket.h"
#endif



void JZDriver::init()
{

#ifdef HAS_JZSerialPort
#if defined(_WIN32)
    JZSerialPortDriver::setInstance(new JZSerialDriverWindows());
#elif defined(__linux__)
    JZSerialPortDriver::setInstance(new JZSerialDriverLinux());
#endif
#endif

#ifdef HAS_JZAudio
#if defined(_WIN32)
    JZAudioDriver::setInstance(new JZAudioDriverWindows());
#elif defined(__linux__)
    JZAudioDriver::setInstance(new JZAudioDriverLinux());
#endif
#endif

#ifdef HAS_JZNet
    JZNetPackInit();
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

#ifdef HAS_JZSensor
    JZSensorDriver::clearInstances();
#endif

#ifdef HAS_JZSerialPort
    // JZSerialPortDriver::setInstance 仅保存指针，这里负责释放已创建的驱动实例
    JZSerialPortDriver::setInstance(nullptr);
#endif

#ifdef HAS_JZOTA
    JZOTADriver::setInstance(nullptr);
#endif

    JZPlatform::setInstance(nullptr);
}