#include "JZWifiAp.h"
#include "JZWifiDriver.h"
#include "JZThread.h"
#include "JZDebug.h"

// ==================== 事件转发 ====================
// JZWifiAp 是 JZWifiDriver 的 friend, 受保护的静态分发方法统一在对象所在线程
// 上执行, 保证信号直连、状态一致 (参考 JZBLEPeripheral)。
void JZWifiAp::onStarted(JZWifiAp *self, const char *ip)
{
    std::string a(ip ? ip : "");
    runOnObject(self, [self, a]() {
        self->m_running = true;
        self->m_ip = a;
        self->sigStarted.emit();
    });
}

void JZWifiAp::onStopped(JZWifiAp *self)
{
    runOnObject(self, [self]() {
        self->m_running = false;
        self->m_ip.clear();
        self->m_stationCount = 0;
        self->sigStopped.emit();
    });
}

void JZWifiAp::onStationConnected(JZWifiAp *self, const char *mac, const char *ip)
{
    std::string m(mac ? mac : ""), i(ip ? ip : "");
    runOnObject(self, [self, m, i]() {
        self->m_stationCount++;
        self->sigStationConnected.emit(m, i);
    });
}

void JZWifiAp::onStationDisconnected(JZWifiAp *self, const char *mac, const char *ip)
{
    std::string m(mac ? mac : ""), i(ip ? ip : "");
    runOnObject(self, [self, m, i]() {
        if (self->m_stationCount > 0)
            self->m_stationCount--;
        self->sigStationDisconnected.emit(m, i);
    });
}

// ==================== JZWifiAp ====================
JZ_OBJECT_IMPL(JZWifiAp, JZObject)

JZWifiAp::JZWifiAp(JZObject *parent)
    : JZObject(parent),
      sigStarted(this), sigStopped(this),
      sigStationConnected(this), sigStationDisconnected(this)
{
}

JZWifiAp::~JZWifiAp()
{
    if (auto *drv = getDriver()) {
        if (m_running)
            drv->apStop(this);
    }
}

JZWifiDriver *JZWifiAp::getDriver() { return JZWifiDriver::instance(); }

// ==================== Access Point (做服务) ====================
void JZWifiAp::start(const JZWifiApConfig &config)
{
    auto *drv = getDriver();
    if (!drv) { jzWarning() << "JZWifiAp: 未设置 WiFi 驱动实例"; return; }
    drv->apStart(this, config);
}

void JZWifiAp::stop()
{
    if (auto *drv = getDriver())
        drv->apStop(this);
}

bool JZWifiAp::isRunning() const { return m_running; }
int  JZWifiAp::connectedStationCount() const { return m_stationCount; }
std::string JZWifiAp::ipAddress() const { return m_ip; }
