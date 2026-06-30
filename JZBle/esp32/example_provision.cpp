// ==================== ESP32 回环自我配网演示（新 JZBle API 版） ====================
// 单个 ESP32 同时运行 Central + Peripheral，扫描自己、连自己、给自己发 WiFi 密码

#include "JZBLEController.h"
#include "JZBLEService.h"
#include "JZBLEDeviceDiscoveryAgent.h"
#include "JZBleDriverEsp32.h"
#include "JZLowEnergyCharacteristic.h"
#include "JZApplication.h"
#include "JZLog.h"
#include "JZTimer.h"

static const JZBluetoothUuid PROV_SVC ("0000fff0-0000-1000-8000-00805f9b34fb");
static const JZBluetoothUuid CHAR_SSID("0000fff1-0000-1000-8000-00805f9b34fb");
static const JZBluetoothUuid CHAR_PWD ("0000fff2-0000-1000-8000-00805f9b34fb");
static const JZBluetoothUuid CHAR_STAT("0000fff3-0000-1000-8000-00805f9b34fb");

// ==================== 1. Peripheral 端: 广播 + 收密码 + 上报状态 ====================
class ProvPeripheral : public JZObject
{
    JZ_OBJECT_DECLARE(ProvPeripheral)

public:
    JZSignal<> sigProvisionDone;
    JZSignal<const std::string&, const std::string&> sigCredentialsReceived; // (ssid, password)

    explicit ProvPeripheral(JZObject *parent = nullptr)
        : JZObject(parent)
        , sigProvisionDone(this)
        , sigCredentialsReceived(this)
        , m_controller(nullptr)
    {
        m_controller = JZBLEController::createPeripheral(this);
        m_controller->addService(PROV_SVC);
        m_controller->addCharacteristic(PROV_SVC, CHAR_SSID, CHC_Write, "");
        m_controller->addCharacteristic(PROV_SVC, CHAR_PWD,  CHC_Write, "");
        m_controller->addCharacteristic(PROV_SVC, CHAR_STAT, CHC_Notify, "");

        connect(&m_controller->sigClientConnected,    this, &ProvPeripheral::onConnected);
        connect(&m_controller->sigClientDisconnected, this, &ProvPeripheral::onDisconnected);
        connect(&m_controller->sigCharacteristicWritten, this, &ProvPeripheral::onCharWritten);
    }

    ~ProvPeripheral() override
    {
        if (m_controller)
        {
            m_controller->stopAdvertising();
            delete m_controller;
            m_controller = nullptr;
        }
    }

    void start()
    {
        LOG_I("[Peripheral] start advertising as 'SELF_PROV'...");
        m_ssid.clear();
        m_password.clear();
        m_controller->startAdvertising("SELF_PROV");
    }

private:
    void onConnected()    { LOG_I("[Peripheral] client connected"); }
    void onDisconnected()
    {
        LOG_I("[Peripheral] client disconnected, clear wifi cred");
        m_ssid.clear();
        m_password.clear();
    }

    void onCharWritten(const std::string &charUuid, const std::string &value)
    {
        LOG_I("[Peripheral] Write Char:%s, len:%zu", charUuid.c_str(), value.size());
        if (value.size() > 64)
        {
            LOG_W("[Peripheral] recv data too long, drop");
            return;
        }

        if (charUuid == CHAR_SSID.toString())
        {
            m_ssid = value;
            LOG_I("[Peripheral] Recv SSID: %s", m_ssid.c_str());
        }
        else if (charUuid == CHAR_PWD.toString())
        {
            m_password = value;
            if (m_ssid.empty())
            {
                LOG_E("[Peripheral] Password received before SSID, invalid order");
                return;
            }
            LOG_I("[Peripheral] credentials complete: %s / ***", m_ssid.c_str());
            sigCredentialsReceived.emit(m_ssid, m_password);

            // 模拟 WiFi 连接成功，通知 Central
            LOG_I("[Peripheral] (simulated) WiFi connected!");
            JZBleAbstractDriver::instance()->peripheralSendNotify(CHAR_STAT.toString(), "connected");
            sigProvisionDone.emit();
        }
    }

    JZBLEController *m_controller;
    std::string m_ssid;
    std::string m_password;
};

JZ_OBJECT_IMPL(ProvPeripheral, JZObject)

// ==================== 2. Central 端: 扫描 + 连接 + 写密码 + 收状态 ====================
class ProvCentral : public JZObject
{
    JZ_OBJECT_DECLARE(ProvCentral)

public:
    JZSignal<> sigProvisionDone;

    ProvCentral(const std::string &ssid, const std::string &password, JZObject *parent = nullptr)
        : JZObject(parent)
        , sigProvisionDone(this)
        , m_agent(nullptr)
        , m_controller(nullptr)
        , m_service(nullptr)
        , m_step(Step::Scanning)
        , m_ssid(ssid)
        , m_password(password)
        , m_scanTargetFound(false)
    {
        m_agent = new JZBLEDeviceDiscoveryAgent(this);
        connect(&m_agent->sigDeviceDiscovered, this, &ProvCentral::onScanResult);
    }

    ~ProvCentral() override
    {
        if (m_controller)
        {
            m_controller->disconnectFromDevice();
            delete m_controller;
            m_controller = nullptr;
        }
    }

    void start()
    {
        if (m_ssid.empty() || m_ssid.size() > 32 || m_password.size() > 64)
        {
            LOG_E("[Central] invalid wifi credential");
            return;
        }
        LOG_I("[Central] start scanning...");
        m_step = Step::Scanning;
        m_scanTargetFound = false;
        m_agent->start();
    }

private:
    enum class Step {
        Scanning,
        Connecting,
        DiscoveringServices,
        WritingSsid,
        WritingPassword,
        WaitingStatus,
        Done
    };

    void onScanResult(const JZBluetoothDeviceInfo &info)
    {
        if (m_step != Step::Scanning || m_scanTargetFound) return;
        LOG_D("[Central] found: %s (rssi=%d)", info.name().c_str(), info.rssi());

        if (info.name() == "SELF_PROV")
        {
            LOG_I("[Central] found target device! connecting...");
            m_scanTargetFound = true;
            m_agent->stop();

            m_controller = JZBLEController::createCentral(info, this);
            connect(&m_controller->sigConnected,         this, &ProvCentral::onConnected);
            connect(&m_controller->sigDiscoveryFinished, this, &ProvCentral::onServicesDiscovered);
            connect(&m_controller->sigError,             this, &ProvCentral::onError);

            m_step = Step::Connecting;
            m_controller->connectToDevice();
        }
    }

    void onConnected()
    {
        LOG_I("[Central] connect success");
        m_step = Step::DiscoveringServices;
        m_controller->discoverServices();
    }

    void onServicesDiscovered()
    {
        LOG_I("[Central] services discovered");
        m_service = m_controller->createServiceObject(PROV_SVC, this);
        connect(&m_service->sigCharacteristicWritten, this, &ProvCentral::onCharWritten);
        connect(&m_service->sigCharacteristicChanged, this, &ProvCentral::onNotify);

        m_step = Step::WritingSsid;
        JZTimer::singleShot(500, this, [this]() {
            writeSsidTask();
        });
    }

    void writeSsidTask()
    {
        if (m_step != Step::WritingSsid) return;
        LOG_I("[Central] writing SSID...");
        m_service->writeCharacteristic(CHAR_SSID, m_ssid, false);
    }

    void onCharWritten(const JZLowEnergyCharacteristic &ch, const std::string &value)
    {
        (void)ch; (void)value;
        if (m_step == Step::WritingSsid)
        {
            m_step = Step::WritingPassword;
            LOG_I("[Central] writing password...");
            m_service->writeCharacteristic(CHAR_PWD, m_password, false);
        }
        else if (m_step == Step::WritingPassword)
        {
            m_step = Step::WaitingStatus;
            LOG_I("[Central] wait peripheral wifi connect notify");
        }
    }

    void onNotify(const JZLowEnergyCharacteristic &ch, const std::string &value)
    {
        (void)ch;
        LOG_I("[Central] recv status notify: %s", value.c_str());
        if (value == "connected" && m_step == Step::WaitingStatus)
        {
            LOG_I("========================================");
            LOG_I("  Self-provision complete!");
            LOG_I("========================================");
            m_step = Step::Done;
            sigProvisionDone.emit();
            m_controller->disconnectFromDevice();
        }
    }

    void onError(int err)
    {
        LOG_E("[Central] error code:%d, msg:%s, current step:%d",
              err, m_controller->errorString().c_str(), static_cast<int>(m_step));
        m_step = Step::Scanning;
        m_scanTargetFound = false;
    }

    JZBLEDeviceDiscoveryAgent *m_agent;
    JZBLEController *m_controller;
    JZBLEService    *m_service;
    Step m_step;
    std::string m_ssid;
    std::string m_password;
    bool m_scanTargetFound;
};

JZ_OBJECT_IMPL(ProvCentral, JZObject)

// ==================== 入口 ====================
int main()
{
    JZApplication app;
    JZBleAbstractDriver::setInstance(new JZBleDriverEsp32());

    // Peripheral: 等待被配网
    ProvPeripheral *phr = new ProvPeripheral();
    connect(&phr->sigCredentialsReceived, [](const std::string &ssid, const std::string &pwd) {
        LOG_I("[WiFi] will connect to: %s", ssid.c_str());
        (void)pwd;
    });
    phr->start();

    // 延长延时，保证BLE广播稳定就绪，降低主从射频冲突概率
    ProvCentral *ctrl = new ProvCentral("MyWiFi", "12345678");
    JZTimer::singleShot(2000, [ctrl]() {
        ctrl->start();
    });

    // 配网完成后退出事件循环释放资源
    connect(ctrl, &ProvCentral::sigProvisionDone, [&app, phr, ctrl]() {
        delete phr;
        delete ctrl;
        app.quit();
    });

    return app.exec();
}
