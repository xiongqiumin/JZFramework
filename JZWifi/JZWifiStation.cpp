#include "JZWifiStation.h"
#include "JZWifiDriver.h"
#include "JZThread.h"
#include "JZDebug.h"

// ==================== 事件转发 ====================
// JZWifiStation 是 JZWifiDriver 的 friend, 受保护的静态分发方法统一在对象所在线程
// 上执行, 保证信号直连、状态一致 (参考 JZBLECentral)。
void JZWifiStation::onConnected(JZObjectPointer self, const char *ip)
{
    JZObjectPointerGuard guard(&self);
    auto *obj = (JZWifiStation *)guard.object();
    if (!obj)
        return;

    std::string a(ip ? ip : "");
    runOnObject(obj, [obj, a]() {
        obj->m_state = JZWifiStation::Connected;
        obj->m_ip = a;
        obj->sigConnected.emit();
    });
}

void JZWifiStation::onDisconnected(JZObjectPointer self)
{
    JZObjectPointerGuard guard(&self);
    auto *obj = (JZWifiStation *)guard.object();
    if (!obj)
        return;

    jzDebug() << "onDisconnected";
    runOnObject(obj, [obj]() {
        obj->m_state = JZWifiStation::None;
        obj->m_ip.clear();
        obj->m_lastRssi = -127;
        obj->sigDisconnected.emit();
    });
}

void JZWifiStation::onError(JZObjectPointer self, int code, const char *msg)
{
    JZObjectPointerGuard guard(&self);
    auto *obj = (JZWifiStation *)guard.object();
    if (!obj)
        return;

    std::string m(msg ? msg : "");
    runOnObject(obj, [obj, code, m]() {
        obj->sigError.emit(code, m);
    });
}

// ==================== JZWifiStation ====================
JZ_OBJECT_IMPL(JZWifiStation, JZObject)

JZWifiStation::JZWifiStation(JZObject *parent)
    : JZObject(parent),
      sigConnected(this), sigDisconnected(this), sigError(this),
      sigRssiChanged(this)
{
}

JZWifiStation::~JZWifiStation()
{
    stopRssiMonitor();
    auto *drv = JZWifiDriver::instance();
    if (drv && m_state == Connected)
    {
        drv->stationDisconnect(this);
        sigDisconnected.emit();
    }
}

// ==================== Station (连接) ====================
void JZWifiStation::connectToAp(const JZWifiConfig &config)
{
    if(m_state != None)
    {
        jzWarning() << "JZWifiStation state is" << m_state << ", can't connect";
        return; 
    }    
    
    auto *drv = JZWifiDriver::instance();
    m_state = Connecting;
    drv->stationConnect(this, config);
}

void JZWifiStation::disconnectFromAp()
{
    if(m_state == None || m_state == Disconnecting)
        return;

    jzDebug() << "disconnectFromAp";
    m_state = Disconnecting;
    auto *drv = JZWifiDriver::instance();
    drv->stationDisconnect(this);
}

bool JZWifiStation::isConnected() const 
{ 
    return m_state == Connected; 
}

JZWifiStation::State JZWifiStation::state() const
{
    return m_state; 
}

std::string JZWifiStation::ipAddress() const { 
    return m_ip; 
}

int JZWifiStation::rssi() const
{
    auto *drv = JZWifiDriver::instance();
    return drv ? drv->stationRssi(const_cast<JZWifiStation *>(this)) : -127;
}

int JZWifiStation::signalStrength() const
{
    int r = rssi();
    if (r >= -50) return 5;
    if (r >= -60) return 4;
    if (r >= -70) return 3;
    if (r >= -80) return 2;
    if (r >= -90) return 1;
    return 0;
}

void JZWifiStation::startRssiMonitor(int intervalMs)
{
    if (m_rssiTimerId != INVAILD_TIMER_ID)
        killTimer(m_rssiTimerId);
    m_rssiTimerId = startTimer(intervalMs > 0 ? intervalMs : 5000);
}

void JZWifiStation::stopRssiMonitor()
{
    if (m_rssiTimerId != INVAILD_TIMER_ID) {
        killTimer(m_rssiTimerId);
        m_rssiTimerId = INVAILD_TIMER_ID;
    }
}

void JZWifiStation::timerEvent(JZTimerEvent *event)
{
    if (event->id() == m_rssiTimerId) {
        if (m_state != Connected)
            return;
        int cur = rssi();
        if (cur != m_lastRssi) {
            m_lastRssi = cur;
            sigRssiChanged.emit(cur);
        }
        return;
    }
    JZObject::timerEvent(event);
}
