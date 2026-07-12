#include "JZWifiAp.h"
#include "JZWifiDriver.h"
#include "JZThread.h"
#include "JZDebug.h"

// ==================== 事件转发 ====================
// JZWifiAp 是 JZWifiDriver 的 friend, 受保护的静态分发方法统一在对象所在线程
// 上执行, 保证信号直连、状态一致 (参考 JZBLEPeripheral)。
void JZWifiAp::onStarted(JZObjectPointer self)
{
    JZObjectPointerGuard guard(&self);
    auto *obj = (JZWifiAp *)guard.object();
    if (!obj)
        return;
    
    runOnObject(obj, [obj]() {
        obj->m_state = Started;
        obj->sigStarted.emit();
    });
}

void JZWifiAp::onStopped(JZObjectPointer self)
{
    JZObjectPointerGuard guard(&self);
    auto *obj = (JZWifiAp *)guard.object();
    if (!obj)
        return;

    runOnObject(obj, [obj]() {
        obj->m_state = None;
        obj->m_stationCount = 0;
        obj->sigStopped.emit();
    });
}

void JZWifiAp::onStationConnected(JZObjectPointer self, const char *mac, const char *ip)
{
    JZObjectPointerGuard guard(&self);
    auto *obj = (JZWifiAp *)guard.object();
    if (!obj)
        return;

    std::string m(mac ? mac : ""), i(ip ? ip : "");
    runOnObject(obj, [obj, m, i]() {
        obj->m_stationCount++;
        obj->sigStationConnected.emit(m, i);
    });
}

void JZWifiAp::onStationDisconnected(JZObjectPointer self, const char *mac, const char *ip)
{
    JZObjectPointerGuard guard(&self);
    auto *obj = (JZWifiAp *)guard.object();
    if (!obj)
        return;

    std::string m(mac ? mac : ""), i(ip ? ip : "");
    runOnObject(obj, [obj, m, i]() {
        if (obj->m_stationCount > 0)
            obj->m_stationCount--;
        obj->sigStationDisconnected.emit(m, i);
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
    auto *drv = JZWifiDriver::instance();
    if (drv)
        drv->apStop(this);
    if (m_state == Started)
    {        
        sigStopped.emit();
    }
}

// ==================== Access Point (做服务) ====================
void JZWifiAp::start(const JZWifiApConfig &config)
{
    m_state = Starting;
    m_config = config;

    auto *drv = JZWifiDriver::instance();
    drv->apStart(this, config);
}

void JZWifiAp::stop()
{
    m_state = Stoping;

    auto *drv = JZWifiDriver::instance();
    drv->apStop(this);
}

bool JZWifiAp::isRunning() const { 
    return (m_state == Starting || m_state == Started); 
}
int  JZWifiAp::connectedStationCount() const { return m_stationCount; }
JZWifiAp::State JZWifiAp::state() const{ return m_state; }
