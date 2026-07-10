#include "JZWifiStation.h"
#include "JZWifiDriver.h"
#include "JZThread.h"
#include "JZDebug.h"

// ==================== 事件转发 ====================
// JZWifiStation 是 JZWifiDriver 的 friend, 受保护的静态分发方法统一在对象所在线程
// 上执行, 保证信号直连、状态一致 (参考 JZBLECentral)。
void JZWifiStation::onConnected(JZWifiStation *self, const char *ip)
{
    std::string a(ip ? ip : "");
    runOnObject(self, [self, a]() {
        self->m_connected = true;
        self->m_ip = a;
        self->sigConnected.emit();
    });
}

void JZWifiStation::onDisconnected(JZWifiStation *self)
{
    runOnObject(self, [self]() {
        self->m_connected = false;
        self->m_ip.clear();
        self->sigDisconnected.emit();
    });
}

void JZWifiStation::onError(JZWifiStation *self, int code, const char *msg)
{
    std::string m(msg ? msg : "");
    runOnObject(self, [self, code, m]() {
        self->sigError.emit(code, m);
    });
}

// ==================== JZWifiStation ====================
JZ_OBJECT_IMPL(JZWifiStation, JZObject)

JZWifiStation::JZWifiStation(JZObject *parent)
    : JZObject(parent),
      sigConnected(this), sigDisconnected(this), sigError(this)
{
}

JZWifiStation::~JZWifiStation()
{
    if (auto *drv = getDriver()) {
        if (m_connected)
            drv->stationDisconnect(this);
    }
}

JZWifiDriver *JZWifiStation::getDriver() { return JZWifiDriver::instance(); }

// ==================== Station (连接) ====================
void JZWifiStation::connect(const JZWifiConfig &config)
{
    auto *drv = getDriver();
    if (!drv) { jzWarning() << "JZWifiStation: 未设置 WiFi 驱动实例"; return; }
    drv->stationConnect(this, config);
}

void JZWifiStation::disconnect()
{
    if (auto *drv = getDriver())
        drv->stationDisconnect(this);
}

bool JZWifiStation::isConnected() const { return m_connected; }
std::string JZWifiStation::ipAddress() const { return m_ip; }
