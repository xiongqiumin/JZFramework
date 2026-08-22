#ifndef JZ_SIM_SERVER_H_
#define JZ_SIM_SERVER_H_

#include "JZObject.h"
#include "JZSimPack.h"
#include "JZHttpServer.h"
#include "JZHttpHandler.h"
#include "JZHttpStaticFiles.h"
#include "json/json.h"
#include <string>
#include <vector>
#include <map>
#include <functional>

class JZHttpServerClient;
class JZSimPack;

// 模拟驱动下行路由回调类型(替代原 JZSimulator 抽象接口, 避免多继承)。
// 注册时由各驱动 simulator 把自己的 support/onRecv 以 lambda 形式绑定进来。
using JZSimSupportFn = std::function<bool(JZSimCmd)>;
using JZSimRecvFn   = std::function<void(const JZSimPack&)>;

// 一个已注册驱动(下行路由)的条目
struct SimEntry { int token; JZSimSupportFn supportFn; JZSimRecvFn recvFn; };

// 模拟器服务器: 内置一个 JZHttpServer, 去掉原来的外部网络连接(JZNetClient).
// 外部通过 HTTP 接口控制模拟器(驱动 driver 收发、触发 Device 侧动作、读取状态),
// Device 侧模拟逻辑(BLE GATT 等)也内置于本对象, 不再依赖 tools/simulator 进程。
//
// 生命周期: 各驱动模拟器构造时调用 start(module) (引用计数 +1, 首个触发 startHttp),
// 析构时调用 stop(module) (引用计数 -1, 归零时 stopHttp)。
class JZSimServer : public JZObject
{
    JZ_OBJECT_DECLARE(JZSimServer)

public:
    static JZSimServer* instance();

    JZSimServer(JZObject* parent = nullptr);
    ~JZSimServer() override;

    // 引用计数启动/停止: 首个 driver 构造时真正 startHttp,
    // 最后一个 driver 析构时 stopHttp。module 仅用于日志区分。
    void start(const std::string& module);
    void stop(const std::string& module);

    // 注册/注销模拟驱动(用于下行路由)。返回值为该驱动的注册令牌, 注销时回传。
    int registerSimulator(JZSimSupportFn supportFn, JZSimRecvFn recvFn);
    void unregisterSimulator(int token);

    // 启动内嵌 HTTP 服务器, 监听指定端口。返回是否成功(一般经 start/stop 间接调用)。
    bool startHttp(int port = 19123);
    // 关闭 HTTP 服务器。
    void stopHttp();
    bool isHttpRunning() const;

    // driver simulator 下行入口: 原实现把指令通过 JZNetClient 发给外部 Device,
    // 现在直接在本进程内交给内置 Device 模拟逻辑处理(handlePack)。
    void sendSim(JZSimCmd cmd, JZVariantMap data = JZVariantMap());

    // ---------------- 关联(供网页端显示) ----------------
    // driver simulator 在 open/启用某资源时调用, 把该资源登记为一条关联记录;
    // 网页端(GET /sim/state)可看到当前所有关联。
    struct AssocInfo {
        std::string category;   // 类别, 如 "gpio" / "ble" / "wifi" / "sensor" / "ota"
        std::string id;         // 该类别下的唯一标识, 如 "pin:5" / 蓝牙地址
        std::string label;      // 展示名
    };
    void addAssoc(const std::string& category, const std::string& id, const std::string& label);
    void removeAssoc(const std::string& category, const std::string& id);

    // ---------------- 外围设备配置(运行时内存, 重启清空) ----------------
    // 网页"加载配置/保存配置"读写; 各驱动访问时按 category 取对应外围设备清单。
    // 结构示例: { "wifi": { "aps": [ {"ssid":..,"password":..,"rssi":..,"bssid":..,"auth":..}, ... ] }, ... }
    const Json::Value& config() const { return m_config; }
    // 写入新配置, 并对新旧配置做 diff, 向对应驱动发送细粒度变更事件(如 WiFi AP 增删改)。
    void setConfig(const Json::Value& cfg);

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
    void handleHttpAssociations(JZHttpServerClient* client);
    // GET /sim/config 返回当前配置; POST /sim/config 解析 body 写入(仅内存)。
    void handleHttpConfig(JZHttpServerClient* client, const JZHttpRequest& req);

    // 配置 diff: 比较新旧 wifi.aps, 针对每个变更发出具体事件(SIM_WIFI_AP_ADDED/
    // REMOVED/UPDATED)。后续其它 category 可在此追加对应 diff 函数。
    void diffWifiConfig(const Json::Value& oldCfg, const Json::Value& newCfg);
    // 按 category 取出对应驱动页面资源(:/simulator/drivers/<cat>.html), 用于内嵌到右侧面板。
    void handleHttpDriverPage(JZHttpServerClient* client, const std::string& query);
    // 根页面: 重定向/转发到 :/simulator/index.html
    void handleHttpIndex(JZHttpServerClient* client);

    // JZVariant / JZVariantMap -> Json::Value(支持 string/int/bool/list/map 嵌套)
    static Json::Value variantToJson(const JZVariant& v);
    static Json::Value variantMapToJson(const JZVariantMap& m);
    static Json::Value variantEventsToJson(const std::vector<JZSimPack>& events);

private:
    // 设备自身维护的 GATT 表(供"作为 peripheral 被 App 的 central 连接"场景使用)
    struct SimChar {
        std::string svc;
        std::string chr;
        int        props = 0;
        std::string value;
        bool       notifying = false;
        SimChar() = default;
        SimChar(std::string s, std::string c, int p, std::string v)
            : svc(std::move(s)), chr(std::move(c)), props(p), value(std::move(v)) {}
    };

    JZHttpServer* m_http = nullptr;
    JZHttpStaticFiles* m_static = nullptr;     // 资源静态文件(:/simulator)
    int m_refCount = 0;                       // start()/stop() 引用计数
    std::vector<SimEntry> m_simulators;    // 已注册驱动(下行路由)
    int m_simTokenSeq = 0;                 // 注册令牌自增序号

    std::string m_devAddr = "AA:BB:CC:DD:EE:FF";
    std::string m_name    = "JZSimDev";
    bool        m_advertising = false;
    std::string m_centralAddr;        // App 作为 central 连上来时的地址
    std::string m_periphClientAddr;   // 设备模拟 central 连到 App peripheral 时的地址
    std::vector<SimChar> m_chars;

    // 当前 HTTP 请求处理期间产生的回发事件(供序列化进响应)。
    std::vector<JZSimPack> m_pendingEvents;

    // 已关联的资源(供网页端显示)。key = category + ":" + id
    std::map<std::string, AssocInfo> m_assocs;

    // 外围设备配置(运行时内存)。按 category 组织各驱动的外围设备清单。
    Json::Value m_config;
};

#endif // !JZ_SIM_SERVER_H_
