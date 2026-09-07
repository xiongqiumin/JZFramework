#include "JZPlatform.h"
#include "JZDriver.h"

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

#ifdef HAS_JZAudio
#include "JZAudio/JZAudioDriver.h"
#endif

#ifdef HAS_JZSerialPort
#include "JZSerialPortDriver.h"
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

#elif defined(HAS_JZSimulator)

#ifdef HAS_JZBle
#include "JZBleDriverSimulator.h"
#endif

#ifdef HAS_JZGpio
#include "JZGpioDriverSimulator.h"
#endif

#ifdef HAS_JZWifi
#include "JZWifiDriverSimulator.h"
#endif

#ifdef HAS_JZSensor
#include "JZSensorDriverSimulator.h"
#endif

#ifdef HAS_JZOTA
#include "JZOTADriverSimulator.h"
#endif

#endif

#ifdef HAS_JZSerialPort
#if defined(_WIN32)
#include "JZSerialDriverWindows.h"
#elif defined(__linux__)
#include "JZSerialDriverLinux.h"
#endif
#endif // HAS_JZSerialPort

#ifdef HAS_JZAudio
#if defined(_WIN32)
#include "JZAudio/detail/windows/JZAudioDriverWindows.h"
#elif defined(__linux__)
#include "JZAudio/detail/linux/JZAudioDriverLinux.h"
#endif
#endif // HAS_JZAudio

#ifdef HAS_JZNet
#include "JZNet/JZNetPacket.h"
#endif

#ifdef HAS_JZWidget
#include "JZGdiBackend.h"
#endif



// ==================== 模块初始化 / 反初始化 ====================
// 平台相关的宏判断收敛在各模块内部，JZDriver::init/deinit 不再包含任何宏。

static void jzDriverInitPlatform()
{
#ifdef ESP_PLATFORM
    JZPlatform::setInstance(new JZPlatformWrapperEsp32());
#endif
}

static void jzDriverDeinitPlatform()
{
    JZPlatform::setInstance(nullptr);
}

static void jzDriverInitBle()
{
#ifdef HAS_JZBle
#ifdef ESP_PLATFORM
    JZBleDriver::setInstance(new JZBleDriverEsp32());
#elif defined(HAS_JZSimulator)
    JZBleDriver::setInstance(new JZBleDriverSimulator());
#endif
#endif
}

static void jzDriverDeinitBle()
{
#ifdef HAS_JZBle
    JZBleDriver::setInstance(nullptr);
#endif
}

static void jzDriverInitWifi()
{
#ifdef HAS_JZWifi
#ifdef ESP_PLATFORM
    JZWifiDriver::setInstance(new JZWifiDriverEsp32());
#elif defined(HAS_JZSimulator)
    JZWifiDriver::setInstance(new JZWifiDriverSimulator());
#endif
#endif
}

static void jzDriverDeinitWifi()
{
#ifdef HAS_JZWifi
    JZWifiDriver::setInstance(nullptr);
#endif
}

static void jzDriverInitGpio()
{
#ifdef HAS_JZGpio
#ifdef ESP_PLATFORM
    JZGpioDriver::addInstance(new JZGpioDriverEsp32());
#elif defined(HAS_JZSimulator)
    JZGpioDriver::addInstance(new JZGpioDriverSimulator());
#endif
#endif
}

static void jzDriverDeinitGpio()
{
#ifdef HAS_JZGpio
    JZGpioDriver::clearInstances();
#endif
}

static void jzDriverInitSensor()
{
#ifdef HAS_JZSensor
#ifdef ESP_PLATFORM
    JZSensorDriver::addInstance(new JZSensorDriverEsp32());
#elif defined(HAS_JZSimulator)
    JZSensorDriver::addInstance(new JZSensorDriverSimulator());
#endif
#endif
}

static void jzDriverDeinitSensor()
{
#ifdef HAS_JZSensor
    JZSensorDriver::clearInstances();
#endif
}

static void jzDriverInitSerialPort()
{
#ifdef HAS_JZSerialPort
#ifdef ESP_PLATFORM
    JZSerialPortDriver::setInstance(new JZSerialDriverEsp32());
#elif defined(_WIN32)
    JZSerialPortDriver::setInstance(new JZSerialDriverWindows());
#elif defined(__linux__)
    JZSerialPortDriver::setInstance(new JZSerialDriverLinux());
#endif
#endif
}

static void jzDriverDeinitSerialPort()
{
#ifdef HAS_JZSerialPort
    // JZSerialPortDriver::setInstance 仅保存指针，这里负责释放已创建的驱动实例
    JZSerialPortDriver::setInstance(nullptr);
#endif
}

static void jzDriverInitOta()
{
#ifdef HAS_JZOTA
#ifdef ESP_PLATFORM
    JZOTADriver::setInstance(new JZOTADriverEsp32());
#elif defined(HAS_JZSimulator)
    JZOTADriver::setInstance(new JZOTADriverSimulator());
#endif
#endif
}

static void jzDriverDeinitOta()
{
#ifdef HAS_JZOTA
    JZOTADriver::setInstance(nullptr);
#endif
}

static void jzDriverInitNet()
{
#ifdef HAS_JZNet
    JZNetPackInit();
#endif
}

static void jzDriverInitAudio()
{
#ifdef HAS_JZAudio
#if defined(_WIN32)
    JZAudioDriver::setInstance(new JZAudioDriverWindows());
#elif defined(__linux__)
    JZAudioDriver::setInstance(new JZAudioDriverLinux());
#endif
#endif
}

static void jzDriverDeinitAudio()
{
#ifdef HAS_JZAudio
    JZAudioDriver::setInstance(nullptr);
#endif
}

static void jzDriverInitWidget()
{
#ifdef HAS_JZWidget
    // 字体度量引擎必须早于任何 UI 构造/布局注册到 JZFontMetrics。
    // 窗口是惰性创建的 (首次 show() 才建 HWND -> JZGdiBackend::init -> setEngine),
    // 若等到那时才注册, 期间跑过的首次布局只能用 estimateWidth 估算
    // (ASCII 仅 0.55em, 比真实测量窄约 25%), 控件被定得偏窄、文字被裁; 而
    // setEngine 本身不触发重排, 事后无法自动纠正。这里提前申请即消除该误差。
    JZGdiBackend::acquireFontEngine();
#endif
}

static void jzDriverDeinitWidget()
{
#ifdef HAS_JZWidget
    // 与 jzDriverInitWidget 对称 (引用计数, 窗口仍存活时不会提前注销)。
    JZGdiBackend::releaseFontEngine();
#endif
}


// ==================== init / deinit ====================

void JZDriver::init()
{
    jzDriverInitPlatform();

    jzDriverInitBle();
    jzDriverInitWifi();
    jzDriverInitGpio();
    jzDriverInitSensor();
    jzDriverInitSerialPort();
    jzDriverInitOta();

    jzDriverInitNet();
    jzDriverInitAudio();
    jzDriverInitWidget();
}

void JZDriver::deinit()
{
    jzDriverDeinitBle();
    jzDriverDeinitWifi();
    jzDriverDeinitGpio();
    jzDriverDeinitSensor();
    jzDriverDeinitSerialPort();
    jzDriverDeinitOta();

    jzDriverDeinitAudio();
    jzDriverDeinitWidget();

    jzDriverDeinitPlatform();
}
