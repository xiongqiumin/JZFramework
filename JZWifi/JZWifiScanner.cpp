#include "JZWifiScanner.h"
#include "JZWifiDriver.h"
#include "JZThread.h"
#include "JZDebug.h"

// ==================== 事件转发 ====================
// JZWifiScanner 是 JZWifiDriver 的 friend, 受保护的静态分发方法统一在对象所在线程
// 上执行, 保证信号直连、状态一致 (参考 JZBLEDeviceDiscoveryAgent)。
void JZWifiScanner::onFinished(JZObjectPointer self, const JZWifiScanResult &r)
{
    JZObjectPointerGuard guard(&self);
    auto *obj = (JZWifiScanner *)guard.object();
    if (!obj)
        return;

    runOnObject(obj, [obj, r]() {
        obj->m_active = false;
        obj->sigFinished.emit(r);
    });
}

void JZWifiScanner::onError(JZObjectPointer self, int code, const char *msg)
{
    JZObjectPointerGuard guard(&self);
    auto *obj = (JZWifiScanner *)guard.object();
    if (!obj)
        return;

    std::string m(msg ? msg : "");
    runOnObject(obj, [obj, code, m]() {
        obj->m_active = false;
        obj->sigError.emit(code, m);
    });
}

// ==================== JZWifiScanner ====================
JZ_OBJECT_IMPL(JZWifiScanner, JZObject)

JZWifiScanner::JZWifiScanner(JZObject *parent)
    : JZObject(parent),
      sigFinished(this), sigError(this)
{
}

JZWifiScanner::~JZWifiScanner()
{
    stop();
}

void JZWifiScanner::start()
{
    m_cfg = JZScanConfig{};   // 默认: 普通扫描(全信道, 不过滤 SSID)
    doStart();
}

void JZWifiScanner::startRoamScan(const std::string &ssid, bool scanAllChannels)
{
    m_cfg = JZScanConfig{};
    m_cfg.roamScan = true;
    m_cfg.ssid = ssid;
    m_cfg.scanAllChannels = scanAllChannels;
    doStart();
}

void JZWifiScanner::doStart()
{
    auto *drv = JZWifiDriver::instance();
    if (m_active)
    {
        jzWarning() << "JZWifiScanner allready stared";
        return;
    }

    m_result = JZWifiScanResult();
    m_active = true;
    drv->scanStart(this, m_cfg);
}

void JZWifiScanner::stop()
{
    if(m_active)
    {
        auto *drv = JZWifiDriver::instance();
        drv->scanStop(this);

        m_active = false;
    }
}

bool JZWifiScanner::isActive() const { return m_active; }

const JZWifiScanResult &JZWifiScanner::result() const
{
    return m_result;
}
