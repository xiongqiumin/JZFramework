#include "JZWifiDriver.h"
#include "JZWifi.h"
#include "JZWifiStation.h"
#include "JZWifiAp.h"
#include "JZWifiScanner.h"
#include "JZDebug.h"

JZWifiDriver *JZWifiDriver::s_instance = nullptr;

JZWifiDriver *JZWifiDriver::instance()
{
    JZ_ASSERT_X(s_instance, "JZWifiDriver not init");
    return s_instance;
}
void JZWifiDriver::setInstance(JZWifiDriver *inst) 
{ 
    if (s_instance && s_instance != inst) 
    {
        if (inst)
            jzWarning("JZWifiDriver instance already set, replacing old driver");
        delete s_instance;
    }
    s_instance = inst; 
}

// ==================== 事件转发中转 ====================
// JZWifiDriver 是各管理类的 friend, 这里把受保护的静态分发方法封装为公开的
// 转发入口, 具体驱动只调用本层, 不直接碰 friend 关系。
void JZWifiDriver::onStationConnected(JZObjectPointer self, const char *ip)
{ JZWifiStation::onConnected(self, ip); }

void JZWifiDriver::onStationDisconnected(JZObjectPointer self)
{ JZWifiStation::onDisconnected(self); }

void JZWifiDriver::onStationError(JZObjectPointer self, int code, const char *msg)
{ JZWifiStation::onError(self, code, msg); }

void JZWifiDriver::onApStarted(JZObjectPointer self)
{ JZWifiAp::onStarted(self); }

void JZWifiDriver::onApStopped(JZObjectPointer self)
{ JZWifiAp::onStopped(self); }

void JZWifiDriver::onApStationConnected(JZObjectPointer self, const char *mac, const char *ip)
{ JZWifiAp::onStationConnected(self, mac, ip); }

void JZWifiDriver::onApStationDisconnected(JZObjectPointer self, const char *mac, const char *ip)
{ JZWifiAp::onStationDisconnected(self, mac, ip); }

void JZWifiDriver::onScanResult(JZObjectPointer self, const JZWifiScanResult &r)
{ JZWifiScanner::onResult(self, r); }

void JZWifiDriver::onScanFinished(JZObjectPointer self)
{ JZWifiScanner::onFinished(self); }

void JZWifiDriver::onScanError(JZObjectPointer self, int code, const char *msg)
{ JZWifiScanner::onError(self, code, msg); }
