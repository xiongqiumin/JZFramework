#include <string>
#include <vector>
#include <algorithm>
#include "JZCo.h"
#include "JZCoBleCentral.h"
#include "JZCoBleService.h"
#include "JZCoBleDiscoveryAgent.h"
#include "JZBLEDeviceInfo.h"
#include "JZLowEnergyCharacteristic.h"
#include "JZDebug.h"
#include "JZApplication.h"
#include "JZCommandLineParser.h"
#include "JZRegExp.h"

using namespace std;

// 校验 BLE 地址格式: XX:XX:XX:XX:XX:XX (每节两位十六进制)
static bool isValidBleAddress(const string &addr)
{
    JZRegExp re("^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$");
    return re.isValid() && re.test(JZString::fromStdString(addr));
}

// 协程式 BLE 中央设备测试
// 用法:
//   test_coBle AA:BB:CC:DD:EE:FF   # 扫描后连接地址匹配的设备 (addr 必选)
class JZCoBleTest : public JZObject
{
public:
    explicit JZCoBleTest(const string &addr) : m_addr(addr) {
        jzco_spawn([this]() {
            centralCoroutine();
            jzApp->quit();
        });
    }

    ~JZCoBleTest() {
        jzco_waitAllCoroutine();
    }

    void centralCoroutine() {
        // 始终先扫描
        JZCoBleDiscoveryAgent agent(this);
        auto sr = agent.scan(8000);
        if (!sr.isOk()) {
            jzDebug() << "扫描失败:" << sr.errorMessage();
            return;
        }

        const vector<JZBLEDeviceInfo> &devices = sr.value();
        jzDebug() << "发现设备数:" << devices.size();
        for (auto &d : devices)
            jzDebug() << "  " << d.address() << " " << d.name();

        if (devices.empty()) {
            jzDebug() << "未发现设备";
            return;
        }

        // 根据 addr 选择要连接的设备
        auto it = find_if(devices.begin(), devices.end(),
                          [this](const JZBLEDeviceInfo &d) {
                              return d.address() == m_addr;
                          });
        if (it == devices.end()) {
            jzDebug() << "未找到地址匹配的设备:" << m_addr;
            return;
        }
        JZBLEDeviceInfo info = *it;

        // 连接设备
        JZCoBleCentral central(this);
        jzDebug() << "连接设备:" << info.address();
        auto r = central.connectToDevice(info, 10000);
        if (!r.isOk()) {
            jzDebug() << "连接失败:" << r.errorMessage();
            return;
        }
        jzDebug() << "连接成功";

        auto ds = central.discoverServices(10000);
        if (!ds.isOk()) {
            jzDebug() << "发现服务失败:" << ds.errorMessage();
            return;
        }

        const vector<JZBluetoothUuid> &services = ds.value();
        jzDebug() << "发现服务数:" << services.size();
        for (auto &uuid : services) {
            jzDebug() << "  service:" << uuid.toString();

            JZCoBleService *svc = central.createService(uuid);
            if (!svc)
                continue;

            auto dr = svc->discoverDetails(5000);
            if (!dr.isOk()) {
                jzDebug() << "  发现特征值失败:" << dr.errorMessage();
                continue;
            }

            const vector<JZLowEnergyCharacteristic> &chars = dr.value();
            jzDebug() << "  特征值数:" << chars.size();
            for (auto &ch : chars) {
                jzDebug() << "    char:" << ch.uuid().toString()
                          << " prop:" << ch.properties();

                // 读取可读特征
                if (ch.properties() & CHC_Read) {
                    auto rr = svc->readCharacteristic(ch.uuid(), 5000);
                    if (rr.isOk())
                        jzDebug() << "      读取值:" << rr.value().size() << " bytes";
                    else
                        jzDebug() << "      读取失败:" << rr.errorMessage();
                }
            }
        }

        jzDebug() << "测试完成，断开连接";
        central.disconnectFromDevice();
    }

protected:
    string m_addr;
};

int main(int argc, char *argv[])
{
    JZApplication app;
    jzco_init();

    string addr;
    JZCommandLineParser parser;
    parser.setDescription("协程式 BLE 测试");
    parser.addArgument("addr", &addr, true);   // 必选：扫描后用于匹配要连接的设备地址
    if (!parser.parse(argc, argv)) {
        parser.printHelp();
        return -1;
    }

    if (!isValidBleAddress(addr)) {
        jzDebug() << "非法的 BLE 地址:" << addr;
        return -1;
    }

    JZCoBleTest test(addr);

    return app.exec();
}
