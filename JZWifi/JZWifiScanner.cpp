#include "JZWifiScanner.h"
#include "JZWifiDriver.h"
#include "JZThread.h"
#include "JZDebug.h"

// ==================== 事件转发 ====================
// JZWifiScanner 是 JZWifiDriver 的 friend, 受保护的静态分发方法统一在对象所在线程
// 上执行, 保证信号直连、状态一致 (参考 JZBLEDeviceDiscoveryAgent)。
void JZWifiScanner::onResult(JZWifiScanner *self, const JZWifiScanResult &r)
{
    JZWifiScanResult c = r;
    runOnObject(self, [self, c]() {
        self->m_results.push_back(c);
        self->sigResult.emit(c);
    });
}

void JZWifiScanner::onFinished(JZWifiScanner *self)
{
    runOnObject(self, [self]() {
        self->m_active = false;
        self->sigFinished.emit();
    });
}

void JZWifiScanner::onError(JZWifiScanner *self, int code, const char *msg)
{
    std::string m(msg ? msg : "");
    runOnObject(self, [self, code, m]() {
        self->m_active = false;
        self->sigError.emit(code, m);
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
    if (!drv) { jzWarning() << "JZWifiScanner: 未设置 WiFi 驱动实例"; return; }
    if (m_active) return;
    m_results.clear();
    m_active = true;
    drv->scanStart(this);
}

void JZWifiScanner::stop()
{
    if (auto *drv = getDriver())
        drv->scanStop(this);
    m_active = false;
}

bool JZWifiScanner::isActive() const { return m_active; }

std::vector<JZWifiScanResult> JZWifiScanner::results() const { return m_results; }
