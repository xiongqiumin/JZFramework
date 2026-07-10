#include "JZWifiDriver.h"
#include "JZWifi.h"
#include "JZWifiStation.h"
#include "JZWifiAp.h"
#include "JZWifiScanner.h"

JZWifiDriver *JZWifiDriver::s_instance = nullptr;

JZWifiDriver *JZWifiDriver::instance()
{
    JZ_ASSERT_X(s_instance, "JZWifiDriver not init");
    return s_instance;
}
void JZWifiDriver::setInstance(JZWifiDriver *inst) { s_instance = inst; }

// ==================== 事件转发中转 ====================
// JZWifiDriver 是各管理类的 friend, 这里把受保护的静态分发方法封装为公开的
// 转发入口, 具体驱动只调用本层, 不直接碰 friend 关系。
void JZWifiDriver::onStationConnected(JZWifiStation *self, const char *ip)
{ JZWifiStation::onConnected(self, ip); }

void JZWifiDriver::onStationDisconnected(JZWifiStation *self)
{ JZWifiStation::onDisconnected(self); }

void JZWifiDriver::onStationError(JZWifiStation *self, int code, const char *msg)
{ JZWifiStation::onError(self, code, msg); }

void JZWifiDriver::onApStarted(JZWifiAp *self, const char *ip)
{ JZWifiAp::onStarted(self, ip); }

void JZWifiDriver::onApStopped(JZWifiAp *self)
{ JZWifiAp::onStopped(self); }

void JZWifiDriver::onApStationConnected(JZWifiAp *self, const char *mac, const char *ip)
{ JZWifiAp::onStationConnected(self, mac, ip); }

void JZWifiDriver::onApStationDisconnected(JZWifiAp *self, const char *mac, const char *ip)
{ JZWifiAp::onStationDisconnected(self, mac, ip); }

void JZWifiDriver::onScanResult(JZWifiScanner *self, const JZWifiScanResult &r)
{ JZWifiScanner::onResult(self, r); }

void JZWifiDriver::onScanFinished(JZWifiScanner *self)
{ JZWifiScanner::onFinished(self); }

void JZWifiDriver::onScanError(JZWifiScanner *self, int code, const char *msg)
{ JZWifiScanner::onError(self, code, msg); }
