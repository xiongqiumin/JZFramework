#ifndef JZ_SIM_SERVER_H_
#define JZ_SIM_SERVER_H_

#include "JZObject.h"
#include "JZSimPack.h"
#include "JZHttpServer.h"
#include "JZHttpHandler.h"
#include "json/json.h"
#include <string>
#include <vector>
#include <map>

class JZHttpServerClient;

// 模拟器服务器: 内置一个 JZHttpServer, 去掉原来的外部网络连接(JZNetClient).
// 外部通过 HTTP 接口控制模拟器(驱动 driver 收发、触发 Device 侧动作、读取状态),
// Device 侧模拟逻辑(BLE GATT 等)也内置于本对象, 不再依赖 tools/simulator 进程。
class JZSimServer : public JZObject
{
    JZ_OBJECT_DECLARE(JZSimServer)

public:
    static JZSimServer* instance();

    JZSimServer(JZObject* parent = nullptr);
    ~JZSimServer() override;

    // 启动内嵌 HTTP 服务器, 监听指定端口。返回是否成功。
    bool startHttp(int port = 19123);
    // 关闭 HTTP 服务器。
    void stopHttp();
    bool isHttpRunning() const;

    // driver simulator 下行入口: 原实现把指令通过 JZNetClient 发给外部 Device,
    // 现在直接在本进程内交给内置 Device 模拟逻辑处理(handlePack)。
    void sendSim(JZSimCmd cmd, JZVariantMap data = JZVariantMap());

protected:
    // 收到"Device -> driver"事件时, 路由到各 driver simulator 的 onRecv。
    void onClientPacket(const JZSimPack& pack);

    // ---------------- 内置 Device 模拟逻辑(合并自 tools/simulator/JZSimulatorDevice) ----------------
    // 处理 driver 下行指令, 模拟硬件行为, 必要时通过 sendToDriver 回发事件。
    void handlePack(const JZSimPack& p);
    // 回发"Device -> driver"事件(直接路由到各 driver simulator 的 onRecv, 并记录到本次请求的事件列表)。
    void sendToDriver(JZSimCmd cmd, const JZVariantMap& m);
    // Device 侧控制动作(对应原 LVGL 界面按钮)。
    void doSetName(const std::string& name);
    void doSetAdvertising(bool on);
    void doSimulateCentralConnect();
    void doDisconnectClient();
    void doNotify(const std::string& svc, const std::string& chr, const std::string& val);
    void doWrite(const std::string& chr, const std::string& val);
    // 设备状态(供 HTTP 读取)。
    Json::Value getStateJson() const;

    // ---------------- HTTP 处理 ----------------
    void handleHttpControl(JZHttpServerClient* client, const JZHttpRequest& req);
    void handleHttpPack(JZHttpServerClient* client, const JZHttpRequest& req);
    void handleHttpState(JZHttpServerClient* client);

    // JZVariant / JZVariantMap -> Json::Value(支持 string/int/bool/list/map 嵌套)
    static Json::Value variantToJson(const JZVariant& v);
    static Json::Value variantMapToJson(const JZVariantMap& m);

private:
    // 设备自身维护的 GATT 表(供"作为 peripheral 被 App 的 central 连接"场景使用)
    struct SimChar {
        std::string svc;
        std::string chr;
        int        props = 0;
        std::string value;
        bool       notifying = false;
    };

    JZHttpServer* m_http = nullptr;

    std::string m_devAddr = "AA:BB:CC:DD:EE:FF";
    std::string m_name    = "JZSimDev";
    bool        m_advertising = false;
    std::string m_centralAddr;        // App 作为 central 连上来时的地址
    std::string m_periphClientAddr;   // 设备模拟 central 连到 App peripheral 时的地址
    std::vector<SimChar> m_chars;

    // 当前 HTTP 请求处理期间产生的回发事件(供序列化进响应)。
    std::vector<JZSimPack> m_pendingEvents;
};

#endif // !JZ_SIMULATOR_CLIENT_H_
