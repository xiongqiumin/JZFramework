#include "JZWifiStation.h"
#include "JZWifiDriver.h"
#include "JZThread.h"
#include "JZDebug.h"

// ==================== 事件转发 ====================
// JZWifiStation 是 JZWifiDriver 的 friend, 受保护的静态分发方法统一在对象所在线程
// 上执行, 保证信号直连、状态一致 (参考 JZBLEController)。
void JZWifiStation::onConnected(JZObjectPointer self, const char *ip, const char *bssid)
{
    JZObjectPointerGuard guard(&self);
    auto *obj = (JZWifiStation *)guard.object();
    if (!obj)
        return;

    std::string a(ip ? ip : "");
    std::string b(bssid ? bssid : "");
    runOnObject(obj, [obj, a, b]() {
        obj->m_state = JZWifiStation::Connected;
        obj->m_ip = a;
        obj->m_bssid = b;     // 缓存连接 AP 的 BSSID(连接事件即带过来, 无需实时查驱动)
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
        // 切换(Switching)过程中收到 onDisconnected = 切换失败: 驱动层断线状态机会
        // 吞掉 esp_wifi_disconnect 触发的中间断开(reason==WIFI_REASON_ASSOC_LEAVE),
        // 只有真正失败(非 ASSOC_LEAVE 掉线或 connect 同步失败)才会走到这里上报。
        obj->m_state = JZWifiStation::None;
        obj->m_ip.clear();
        obj->m_bssid.clear();        
        obj->sigDisconnected.emit();
    });
}

void JZWifiStation::onStationSwitched(JZObjectPointer self, bool ok, const char *bssid)
{
    JZObjectPointerGuard guard(&self);
    auto *obj = (JZWifiStation *)guard.object();
    if (!obj)
        return;

    // 切换结果(成功/失败)经本回调上报, 在对象线程更新状态并发 sigSwitched。
    // 注意: 切换过程中驱动层不会另发 sigConnected/sigDisconnected, 由本回调统一代表切换结果。
    std::string b(bssid ? bssid : "");
    runOnObject(obj, [obj, ok, b]() {
        if (ok) {
            // 切换成功: 回到 Connected, 更新 BSSID(新 AP), IP 一般不变(同子网)予以保留
            obj->m_state = JZWifiStation::Connected;
            obj->m_bssid = b;
            obj->sigSwitched.emit();
        } else {
            // 切换失败: 未连上任何 AP, 回到 None
            int pre_state = obj->m_state;
            obj->m_state = JZWifiStation::None;
            obj->m_ip.clear();
            obj->m_bssid.clear();
            if(obj->m_state == JZWifiStation::Connected)
                obj->sigDisconnected.emit();
        }        
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
      sigConnected(this), sigDisconnected(this), sigSwitched(this), sigError(this)
{
}

JZWifiStation::~JZWifiStation()
{    
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

void JZWifiStation::switchToAp(const JZWifiConfig &config)
{
    if (m_state != Connected) {
        jzWarning() << "JZWifiStation state is" << m_state << ", can't switch";
        return;
    }

    auto *drv = JZWifiDriver::instance();
    if (!drv)
        return;

    jzDebug() << "switchToAp" << config.ssid << config.bssid;
    m_state = Switching;          // 切换中: 驱动层会吞掉 esp_wifi_disconnect 触发的
                                   // 中间断开(reason==WIFI_REASON_ASSOC_LEAVE), 真实失败才回调
    drv->stationSwitch(this, config);
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

int JZWifiStation::currentRssi() const
{
    auto *drv = JZWifiDriver::instance();
    return drv ? drv->stationRssi(const_cast<JZWifiStation *>(this)) : -127;
}

std::string JZWifiStation::bssid() const
{
    // 返回连接事件缓存的 BSSID, 无需实时查询驱动(扫描结果/连接事件均已携带 bssid)
    return m_bssid;
}

int JZWifiStation::signalStrength() const
{
    int r = currentRssi();
    if (r >= -50) return 5;
    if (r >= -60) return 4;
    if (r >= -70) return 3;
    if (r >= -80) return 2;
    if (r >= -90) return 1;
    return 0;
}
