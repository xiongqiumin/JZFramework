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
      sigResult(this), sigFinished(this), sigError(this)
{
}

JZWifiScanner::~JZWifiScanner()
{
    stop();
}

JZWifiDriver *JZWifiScanner::getDriver() { return JZWifiDriver::instance(); }

void JZWifiScanner::start()
{
    auto *drv = getDriver();    
    if (m_active) 
    {
        jzWarning() << "JZWifiScanner allready stared";
        return;
    }

    m_results.clear();
    m_active = true;
    drv->scanStart(this);
}

void JZWifiScanner::stop()
{
    auto *drv = getDriver();
    drv->scanStop(this);

    m_active = false;
}

bool JZWifiScanner::isActive() const { return m_active; }

std::vector<JZWifiScanResult> JZWifiScanner::results() const { 
    return m_results; 
}
