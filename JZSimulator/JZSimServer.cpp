#include "JZSimServer.h"
#include "JZApplication.h"

#include "JZDataStream.h"
#include "JZByteArray.h"
#include "JZLog.h"
#include "JZDebug.h"
#include "JZJson.h"
#include "JZStd.h"

// 网页静态资源(由 JZSimulator/http/simulator.qrc 经 jz_add_resources 生成)
#include "jz_simulator_resources.h"

#include <algorithm>
#include <set>
#include <json/reader.h>
#include <json/writer.h>

// 由 cmd 推断所属驱动类别(与各 driver 注册时 support 回调的区间约定保持一致)
static std::string categoryOf(JZSimCmd cmd)
{
    int c = (int)cmd;
    if (c >= 0   && c < 100) return "ble";
    if (c >= 100 && c < 200) return "gpio";
    if (c >= 200 && c < 300) return "wifi";
    if (c >= 400 && c < 500) return "sensor";
    return "ble";
}

// 当前已注册(即 Has_xxx 启用)的驱动类别列表
static std::vector<std::string> availableCategories(const std::vector<SimEntry>& sims)
{
    std::set<std::string> cats;
    // 用各 driver 自身 support() 判断其覆盖区间, 推断类别
    const JZSimCmd probes[] = { SIM_CMD_SCAN_START, SIM_GPIO_CMD_OPEN,
                                 SIM_WIFI_AP_START, SIM_SENSOR_CMD_BASE };
    for (auto& sim : sims) {
        if (!sim.supportFn) continue;
        for (JZSimCmd c : probes) {
            if (sim.supportFn(c))
                cats.insert(categoryOf(c));
        }
    }
    return std::vector<std::string>(cats.begin(), cats.end());
}

// JSON / Variant 辅助(自由函数)
static JZVariant jsonToVariant(const Json::Value& v);
static Json::Value variantEventsToJson(const std::vector<JZSimPack>& events);
static JZByteArray jsonResponse(const Json::Value& v);

// ===================== JZSimServer =====================
JZ_OBJECT_IMPL(JZSimServer, JZObject)

static JZSimServer* g_simInstance = nullptr;

JZSimServer* JZSimServer::instance()
{
    if (!g_simInstance)
        g_simInstance = new JZSimServer(jzApp);
    return g_simInstance;
}

JZSimServer::JZSimServer(JZObject* parent)
    : JZObject(parent)
{
    JZSimPack::ensureRegistered();

    // 预置 GATT 表, 供 App 的 central 发现/读写
    m_chars.push_back({"0000FFF0-0000-1000-8000-00805f9b34fb",
                       "0000FFF1-0000-1000-8000-00805f9b34fb", 0x01 | 0x02 | 0x04, "hello"});
    m_chars.push_back({"0000FFF0-0000-1000-8000-00805f9b34fb",
                       "0000FFF2-0000-1000-8000-00805f9b34fb", 0x01 | 0x02, "world"});
    m_chars.push_back({"0000FFB0-0000-1000-8000-00805f9b34fb",
                       "0000FFB1-0000-1000-8000-00805f9b34fb", 0x01 | 0x04, "1234"});

    // 预置 WiFi 默认配置: 无外部配置时扫描也能返回可用 AP。
    // 该默认 AP 同时作为 Station 连接的固定凭证(ssid/password)来源, 不再在驱动内硬编码。
    Json::Value ap(Json::objectValue);
    ap["ssid"]  = "jz_simulator_ap";
    ap["password"] = "123456";
    ap["bssid"] = "00:11:22:33:44:55";
    ap["rssi"]  = -50;
    ap["channel"] = 6;
    ap["auth"]  = 0;   // OPEN
    m_config["wifi"]["aps"].append(ap);
}

JZSimServer::~JZSimServer()
{
    stopHttp();
}

bool JZSimServer::startHttp(int port)
{
    if (m_http)
        return true;

    // 注册编译进二进制的网页资源(:/simulator/...)
    static bool s_resInited = false;
    if (!s_resInited) {
        jzres_simulator_init();
        s_resInited = true;
    }
    m_static = new JZHttpStaticFiles(":/simulator");

    m_http = new JZHttpServer(this);

    // 内部 handler: 把请求分派到 JZSimServer 的公开处理方法。
    class JZSimHttpHandler : public JZHttpHandler
    {
    public:
        JZSimServer* sim = nullptr;
        void handle(JZHttpServerClient* client, const JZHttpRequest& req) override
        {
            std::string path = req.path();
            // 去掉查询串
            auto q = path.find('?');
            if (q != std::string::npos)
                path = path.substr(0, q);

            if (path == "/sim/control")
                sim->handleHttpControl(client, req);
            else if (path == "/sim/pack")
                sim->handleHttpPack(client, req);
            else if (path == "/sim/state")
                sim->handleHttpState(client);
            else if (path == "/sim/associations")
                sim->handleHttpAssociations(client);
            else if (path == "/sim/config")
                sim->handleHttpConfig(client, req);
            else if (path == "/" || path == "/index.html" || path == "/sim" || path == "/sim/")
                sim->handleHttpIndex(client);
            else if (str_startsWith(path, "/drivers/"))
                sim->handleHttpDriverPage(client, path.substr(std::string("/drivers/").size()));
            else if (str_startsWith(path, "/css/") || str_startsWith(path, "/js/")
                     || str_startsWith(path, "/drivers/") || path == "/index.html")
            {
                // 相对路径资源(:/simulator 前缀下), 直接交给资源静态处理器
                JZHttpRequest sub = req;
                sim->m_static->handle(client, sub);
            }
            else
            {
                JZHttpRespone resp;
                resp.setStatusCode(404);
                resp.setJson(R"({"error":"not found"})");
                client->response(resp.toByteArray());
            }
        }
    };

    auto* h = new JZSimHttpHandler();
    h->sim = this;
    m_http->addRoute("/sim/control", h);
    m_http->addRoute("/sim/pack", h);
    m_http->addRoute("/sim/state", h);
    m_http->addRoute("/sim/associations", h);
    m_http->addRoute("/sim/config", h);
    m_http->addRoute("/drivers", h);
    m_http->addRoute("/sim/static", h);
    m_http->addRoute("/sim", h);
    m_http->addRoute("/", h);

    if (!m_http->listen(port))
    {
        jzWarning() << "JZSimServer: HTTP server listen failed on port" << port;
        delete m_http;
        m_http = nullptr;
        return false;
    }

    return true;
}

void JZSimServer::stopHttp()
{
    if (m_http)
    {
        m_http->close();
        delete m_http;
        m_http = nullptr;
    }
    if (m_static)
    {
        delete m_static;
        m_static = nullptr;
    }
}

bool JZSimServer::isHttpRunning() const
{
    return m_http && m_http->isListening();
}

void JZSimServer::sendSim(JZSimCmd cmd, JZVariantMap data)
{
    JZSimPack pack;
    pack.cmd = cmd;
    pack.data = data;
    handlePack(pack);
}

// ===================== 引用计数启动/停止 =====================
void JZSimServer::start(const std::string& module)
{
    m_refCount++;
    if (m_refCount == 1)
        startHttp();
}

void JZSimServer::stop(const std::string& module)
{
    if (m_refCount > 0)
        m_refCount--;

    if (m_refCount == 0)
        stopHttp();
}

int JZSimServer::registerSimulator(JZSimSupportFn supportFn, JZSimRecvFn recvFn)
{
    int token = ++m_simTokenSeq;
    m_simulators.push_back({token, supportFn, recvFn});
    return token;
}

void JZSimServer::unregisterSimulator(int token)
{
    for (auto it = m_simulators.begin(); it != m_simulators.end(); ++it)
    {
        if (it->token == token)
        {
            m_simulators.erase(it);
            return;
        }
    }
}

// 收到"Device -> driver"事件时, 路由到各 driver simulator 的 onRecv。
void JZSimServer::onClientPacket(const JZSimPack& pack)
{
    for (auto& sim : m_simulators)
    {
        if (sim.supportFn && sim.supportFn(pack.cmd) && sim.recvFn)
            sim.recvFn(pack);
    }
}

// ===================== Device 模拟: 上行事件回发 =====================
void JZSimServer::sendToDriver(JZSimCmd cmd, const JZVariantMap& m)
{
    JZSimPack pack;
    pack.cmd = cmd;
    pack.data = m;
    // 直接路由到各 driver simulator 的 onRecv(Device -> driver 方向)
    onClientPacket(pack);
    // 记录到本次请求的事件列表, 供 HTTP 响应返回
    m_pendingEvents.push_back(pack);
}

// ===================== Device 模拟: 下行指令处理 =====================
void JZSimServer::handlePack(const JZSimPack& p)
{
    JZSimCmd cmd = p.cmd;

    switch (cmd) {
    case SIM_CMD_SCAN_START: {
        if (!m_advertising) { jzDebug() << "scan ignored (not advertising)"; break; }
        std::vector<std::string> su;
        for (auto& ch : m_chars) if (!vec_contains(su, ch.svc)) su.push_back(ch.svc);
        JZVariantMap m;
        m["addr"] = JZVariant(m_devAddr); m["name"] = JZVariant(m_name);
        m["rssi"] = JZVariant(-40); {
            JZVariantList _l; _l.reserve(su.size());
            for (auto& _e : su) _l.push_back(JZVariant(_e));
            m["su"] = JZVariant::fromValue(_l);
        }
        sendToDriver(SIM_EVT_SCAN_RESULT, m);
        jzDebug() << "scan -> reported" << m_name;
        break;
    }
    case SIM_CMD_SCAN_STOP:
        break;
    case SIM_CMD_CONNECT: {
        std::string addr = p.data.at("addr").toString();
        m_centralAddr = addr;
        JZVariantMap m; m["addr"] = JZVariant(addr); m["role"] = JZVariant(0);
        sendToDriver(SIM_EVT_CONNECTED, m);
        jzDebug() << "central connected:" << addr;
        break;
    }
    case SIM_CMD_DISCONNECT: {
        std::string addr = p.data.at("addr").toString();
        if (addr == m_centralAddr) {
            m_centralAddr.clear();
            JZVariantMap m; m["addr"] = JZVariant(addr); m["role"] = JZVariant(0);
            sendToDriver(SIM_EVT_DISCONNECTED, m);
            jzDebug() << "central disconnected:" << addr;
        }
        break;
    }
    case SIM_CMD_DISCOVER_SVC: {
        std::string addr = p.data.at("addr").toString();
        std::vector<std::string> su;
        for (auto& ch : m_chars) if (!vec_contains(su, ch.svc)) su.push_back(ch.svc);
        JZVariantMap m; m["addr"] = JZVariant(addr); {
            JZVariantList _l; _l.reserve(su.size());
            for (auto& _e : su) _l.push_back(JZVariant(_e));
            m["su"] = JZVariant::fromValue(_l);
        }
        sendToDriver(SIM_EVT_SERVICES, m);
        break;
    }
    case SIM_CMD_DISCOVER_CHR: {
        std::string addr = p.data.at("addr").toString();
        std::string svc = p.data.at("svc").toString();
        std::vector<std::string> cls;
        for (auto& ch : m_chars)
            if (ch.svc == svc) cls.push_back(ch.chr + "|" + std::to_string(ch.props));
        JZVariantMap m; m["addr"] = JZVariant(addr); m["svc"] = JZVariant(svc); {
            JZVariantList _l; _l.reserve(cls.size());
            for (auto& _e : cls) _l.push_back(JZVariant(_e));
            m["chars"] = JZVariant::fromValue(_l);
        }
        sendToDriver(SIM_EVT_CHARACTERISTICS, m);
        break;
    }
    case SIM_CMD_READ: {
        std::string addr = p.data.at("addr").toString();
        std::string svc = p.data.at("svc").toString();
        std::string chr = p.data.at("chr").toString();
        std::string val;
        for (auto& ch : m_chars) if (ch.svc == svc && ch.chr == chr) { val = ch.value; break; }
        JZVariantMap m; m["addr"] = JZVariant(addr); m["svc"] = JZVariant(svc);
        m["chr"] = JZVariant(chr); m["val"] = JZVariant(val);
        sendToDriver(SIM_EVT_READ_RESULT, m);
        break;
    }
    case SIM_CMD_WRITE: {
        std::string addr = p.data.at("addr").toString();
        std::string svc = p.data.at("svc").toString();
        std::string chr = p.data.at("chr").toString();
        std::string val = p.data.at("val").toString();
        for (auto& ch : m_chars) if (ch.svc == svc && ch.chr == chr) ch.value = val;
        jzDebug() << "central wrote" << chr << "=" << val;
        JZVariantMap m; m["addr"] = JZVariant(addr); m["svc"] = JZVariant(svc); m["chr"] = JZVariant(chr);
        sendToDriver(SIM_EVT_WRITE_RESULT, m);
        break;
    }
    case SIM_CMD_ENABLE_NOTIFY: {
        std::string addr = p.data.at("addr").toString();
        std::string svc = p.data.at("svc").toString();
        std::string chr = p.data.at("chr").toString();
        int en = p.data.at("en").toInt();
        for (auto& ch : m_chars) if (ch.svc == svc && ch.chr == chr) ch.notifying = (en != 0);
        jzDebug() << "notify" << (en ? "enabled" : "disabled") << chr;
        break;
    }
    case SIM_CMD_ADD_PERIPH_CHR: {
        std::string chr = p.data.at("chr").toString();
        jzDebug() << "app peripheral added char" << chr;
        break;
    }
    case SIM_CMD_START_ADV: {
        std::string name = p.data.at("name").toString();
        jzDebug() << "app started advertising as peripheral:" << name;
        break;
    }
    case SIM_CMD_STOP_ADV:
        jzDebug() << "app stopped advertising";
        break;
    case SIM_CMD_SEND_NOTIFY: {
        std::string chr = p.data.at("chr").toString();
        std::string val = p.data.at("val").toString();
        jzDebug() << "app peripheral notified" << chr << "=" << val;
        break;
    }
    case SIM_CMD_SEND_INDICATE: {
        std::string chr = p.data.at("chr").toString();
        std::string val = p.data.at("val").toString();
        jzDebug() << "app peripheral indicated" << chr << "=" << val;
        break;
    }
    case SIM_WIFI_STATION_CONNECT: {
        // 校验凭证: 必须与当前 wifi.aps 配置中某个 AP 的 ssid/password 一致。
        std::string ssid = p.data.at("ssid").toString();
        std::string pwd  = p.data.at("password").toString();
        std::string bssid = p.data.at("bssid").toString();
        bool matched = false;
        const Json::Value& wifiCfg = m_config["wifi"];
        const Json::Value& aps = (wifiCfg.isObject() && wifiCfg.isMember("aps")) ? wifiCfg["aps"] : Json::Value(Json::arrayValue);
        if (aps.isArray())
        {
            for (auto it = aps.begin(); it != aps.end(); ++it)
            {
                const Json::Value& a = *it;
                std::string aSsid = a.isMember("ssid") ? a["ssid"].asString() : std::string();
                std::string aPwd  = a.isMember("password") ? a["password"].asString() : std::string();
                if (aSsid == ssid && aPwd == pwd)
                {
                    matched = true;
                    if (bssid.empty() && a.isMember("bssid"))
                        bssid = a["bssid"].asString();
                    break;
                }
            }
        }
        JZVariantMap m;
        m["ok"] = JZVariant(matched);
        m["ssid"] = JZVariant(ssid);
        m["password"] = JZVariant(pwd);
        if (matched)
            m["bssid"] = JZVariant(bssid);
        else
            m["reason"] = JZVariant(std::string("invalid ssid or password"));
        sendToDriver(SIM_WIFI_STATION_RESULT, m);
        jzDebug() << "wifi station connect ->" << (matched ? "ok" : "fail") << ssid;
        break;
    }
    case SIM_WIFI_STATION_DISCONNECT: {
        jzDebug() << "wifi station disconnect";
        break;
    }
    default:
        break;
    }
}

// ===================== Device 模拟: 控制动作 =====================
void JZSimServer::doSetName(const std::string& name)
{
    m_name = name;
    jzDebug() << "name ->" << m_name;
}

void JZSimServer::doSetAdvertising(bool on)
{
    m_advertising = on;
    jzDebug() << (on ? "advertising ON" : "advertising OFF");
}

void JZSimServer::doSimulateCentralConnect()
{
    if (!m_periphClientAddr.empty()) { jzDebug() << "client already connected"; return; }
    m_periphClientAddr = "11:22:33:44:55:66";
    JZVariantMap m; m["addr"] = JZVariant(m_periphClientAddr); m["role"] = JZVariant(1);
    sendToDriver(SIM_EVT_CONNECTED, m);
    jzDebug() << "simulated central connect -> app peripheral";
}

void JZSimServer::doDisconnectClient()
{
    if (m_periphClientAddr.empty()) return;
    JZVariantMap m; m["addr"] = JZVariant(m_periphClientAddr); m["role"] = JZVariant(1);
    sendToDriver(SIM_EVT_DISCONNECTED, m);
    jzDebug() << "simulated central disconnect";
    m_periphClientAddr.clear();
}

void JZSimServer::doNotify(const std::string& svc, const std::string& chr, const std::string& val)
{
    if (m_centralAddr.empty()) { jzDebug() << "Notify: no central connected"; return; }
    std::string s = svc.empty() ? m_chars.empty() ? "" : m_chars[0].svc : svc;
    for (auto& ch : m_chars) if (ch.svc == s && ch.chr == chr) ch.value = val;
    JZVariantMap m;
    m["addr"] = JZVariant(m_centralAddr); m["svc"] = JZVariant(s);
    m["chr"] = JZVariant(chr); m["val"] = JZVariant(val);
    sendToDriver(SIM_EVT_NOTIFY_RX, m);
    jzDebug() << "Notify" << chr << "=" << val;
}

void JZSimServer::doWrite(const std::string& chr, const std::string& val)
{
    if (m_periphClientAddr.empty()) { jzDebug() << "Write: no app-peripheral client"; return; }
    for (auto& ch : m_chars) if (ch.chr == chr) ch.value = val;
    JZVariantMap m;
    m["chr"] = JZVariant(chr); m["val"] = JZVariant(val);
    sendToDriver(SIM_EVT_CHAR_WRITTEN, m);
    jzDebug() << "Write" << chr << "=" << val;
}

Json::Value JZSimServer::getStateJson() const
{
    Json::Value root;
    root["devAddr"] = m_devAddr;
    root["name"] = m_name;
    root["advertising"] = m_advertising;
    root["centralAddr"] = m_centralAddr;
    root["periphClientAddr"] = m_periphClientAddr;

    Json::Value chars(Json::arrayValue);
    for (auto& ch : m_chars) {
        Json::Value c;
        c["svc"] = ch.svc;
        c["chr"] = ch.chr;
        c["props"] = ch.props;
        c["value"] = ch.value;
        c["notifying"] = ch.notifying;
        chars.append(c);
    }
    root["chars"] = chars;

    // 关联资源(供网页端显示)
    Json::Value assocs(Json::arrayValue);
    for (auto& kv : m_assocs) {
        Json::Value a;
        a["category"] = kv.second.category;
        a["id"] = kv.second.id;
        a["label"] = kv.second.label;
        assocs.append(a);
    }
    root["associations"] = assocs;

    // 当前启用的驱动类别(Has_xxx 决定: 仅注册到 JZSimServer 的驱动才出现)
    Json::Value drivers(Json::arrayValue);
    for (auto& cat : availableCategories(m_simulators))
        drivers.append(cat);
    root["availableDrivers"] = drivers;

    // 全部驱动类别(网页默认全显示, 服务端把未启用的放进 disabledDrivers 隐藏)
    const char* allCats[] = { "gpio", "ble", "wifi", "sensor" };
    Json::Value disabled(Json::arrayValue);
    std::set<std::string> enabled;
    for (auto it = drivers.begin(); it != drivers.end(); ++it)
        enabled.insert(it->asString());
    for (auto* c : allCats)
        if (!enabled.count(c))
            disabled.append(c);
    root["disabledDrivers"] = disabled;
    root["config"] = m_config;
    return root;
}

void JZSimServer::addAssoc(const std::string& category, const std::string& id, const std::string& label)
{
    AssocInfo info;
    info.category = category;
    info.id = id;
    info.label = label;
    m_assocs[category + ":" + id] = info;
    jzDebug() << "assoc +" << category << ":" << id << "(" << label << ")";
}

void JZSimServer::removeAssoc(const std::string& category, const std::string& id)
{
    m_assocs.erase(category + ":" + id);
    jzDebug() << "assoc -" << category << ":" << id;
}

void JZSimServer::setConfig(const Json::Value& cfg)
{
    // 先对新旧配置做 diff, 针对具体变更向各驱动发送语义化事件(而不是笼统的"配置变了")。
    diffWifiConfig(m_config, cfg);
    // 其它 category(gpio/sensor/ble)的 diff 可在此追加。

    m_config = cfg;
    jzDebug() << "JZSimServer: config updated (in-memory)";
}

// 取 ap 的唯一标识: 优先 bssid, 否则回退 ssid(同一 ssid 多 AP 场景下以 bssid 区分)。
static std::string apKey(const Json::Value& ap)
{
    if (ap.isMember("bssid") && !ap["bssid"].asString().empty())
        return ap["bssid"].asString();
    return ap.isMember("ssid") ? ap["ssid"].asString() : std::string();
}

// 比较新旧 wifi.aps, 发出 SIM_WIFI_AP_ADDED / REMOVED / UPDATED。
void JZSimServer::diffWifiConfig(const Json::Value& oldCfg, const Json::Value& newCfg)
{
    const Json::Value empty;
    const Json::Value& oldAps = (oldCfg.isMember("wifi") && oldCfg["wifi"].isMember("aps"))
                                ? oldCfg["wifi"]["aps"] : empty;
    const Json::Value& newAps = (newCfg.isMember("wifi") && newCfg["wifi"].isMember("aps"))
                                ? newCfg["wifi"]["aps"] : empty;

    // 旧 ap 按 key 建索引
    std::map<std::string, const Json::Value*> oldMap;
    for (const auto& a : oldAps)
        oldMap[apKey(a)] = &a;

    // 遍历新 ap: 新增 or 更新
    for (const auto& a : newAps)
    {
        std::string key = apKey(a);
        auto it = oldMap.find(key);
        if (it == oldMap.end())
        {
            JZSimPack pack;
            pack.cmd = SIM_WIFI_AP_ADDED;
            pack.json = a;
            onClientPacket(pack);
        }
        else
        {
            // 已存在: 逐字段比较, 任意字段不同则视为 UPDATED(携带新对象)。
            const Json::Value& o = *it->second;
            bool changed = (o.size() != a.size());
            for (const auto& name : a.getMemberNames())
            {
                if (!o.isMember(name) || o[name].toStyledString() != a[name].toStyledString())
                {
                    changed = true;
                    break;
                }
            }
            if (changed)
            {
                JZSimPack pack;
                pack.cmd = SIM_WIFI_AP_UPDATED;
                pack.json = a;
                onClientPacket(pack);
            }
            oldMap.erase(it);
        }
    }

    // oldMap 中剩余的即为被删除的 ap
    for (const auto& kv : oldMap)
    {
        JZSimPack pack;
        pack.cmd = SIM_WIFI_AP_REMOVED;
        pack.json = *kv.second;
        onClientPacket(pack);
    }
}

// ===================== HTTP 处理 =====================
void JZSimServer::handleHttpControl(JZHttpServerClient* client, const JZHttpRequest& req)
{
    Json::Value body; Json::Reader reader;
    if (req.body().size() != 0) {
        reader.parse(std::string(req.body().data(), req.body().size()), body);
    }

    std::string action = body.isMember("action") ? body["action"].asString() : "";
    m_pendingEvents.clear();

    if (action == "setName") {
        doSetName(body.isMember("name") ? body["name"].asString() : m_name);
    } else if (action == "setAdvertising") {
        doSetAdvertising(body.isMember("on") ? body["on"].asBool() : false);
    } else if (action == "simulateCentralConnect") {
        doSimulateCentralConnect();
    } else if (action == "disconnectClient") {
        doDisconnectClient();
    } else if (action == "notify") {
        std::string svc = body.isMember("svc") ? body["svc"].asString() : "";
        std::string chr = body.isMember("chr") ? body["chr"].asString() : "";
        std::string val = body.isMember("val") ? body["val"].asString() : "";
        doNotify(svc, chr, val);
    } else if (action == "write") {
        std::string chr = body.isMember("chr") ? body["chr"].asString() : "";
        std::string val = body.isMember("val") ? body["val"].asString() : "";
        doWrite(chr, val);
    } else {
        JZHttpRespone resp;
        resp.setStatusCode(400);
        resp.setJson(R"({"error":"unknown action"})");
        client->response(resp.toByteArray());
        return;
    }

    Json::Value out;
    out["status"] = "ok";
    out["events"] = JZSimServer::variantEventsToJson(m_pendingEvents);
    out["state"] = getStateJson();
    client->response(jsonResponse(out));
}

void JZSimServer::handleHttpPack(JZHttpServerClient* client, const JZHttpRequest& req)
{
    // 外部直接驱动 "Device -> driver" 事件(例如从网页触发应用侧收到 notify)
    Json::Value body; Json::Reader reader;
    if (req.body().size() == 0 || !reader.parse(std::string(req.body().data(), req.body().size()), body)) {
        JZHttpRespone resp;
        resp.setStatusCode(400);
        resp.setJson(R"({"error":"invalid json"})");
        client->response(resp.toByteArray());
        return;
    }

    int cmd = body.isMember("cmd") ? body["cmd"].asInt() : -1;
    JZVariantMap data;
    if (body.isMember("data") && body["data"].isObject()) {
        const Json::Value& d = body["data"];
        for (auto it = d.begin(); it != d.end(); ++it)
            data[it.name()] = jsonToVariant(*it);
    }

    m_pendingEvents.clear();
    JZSimPack pack; pack.cmd = (JZSimCmd)cmd; pack.data = data;
    onClientPacket(pack);
    m_pendingEvents.push_back(pack);

    Json::Value out;
    out["status"] = "ok";
    out["events"] = JZSimServer::variantEventsToJson(m_pendingEvents);
    client->response(jsonResponse(out));
}

void JZSimServer::handleHttpState(JZHttpServerClient* client)
{
    Json::Value out;
    out["status"] = "ok";
    out["httpRunning"] = isHttpRunning();
    out["state"] = getStateJson();
    client->response(jsonResponse(out));
}

void JZSimServer::handleHttpAssociations(JZHttpServerClient* client)
{
    Json::Value out;
    out["status"] = "ok";
    Json::Value assocs(Json::arrayValue);
    for (auto& kv : m_assocs) {
        Json::Value a;
        a["category"] = kv.second.category;
        a["id"] = kv.second.id;
        a["label"] = kv.second.label;
        assocs.append(a);
    }
    out["associations"] = assocs;
    client->response(jsonResponse(out));
}

// GET /sim/config -> 返回当前配置(运行时内存)
// POST /sim/config -> 解析 body(JSON) 写入配置, 返回写入后的配置
void JZSimServer::handleHttpConfig(JZHttpServerClient* client, const JZHttpRequest& req)
{
    if (req.method() == JZHttpRequest::POST)
    {
        const std::string body = str_fromUtf8(req.body());
        Json::Value cfg;
        Json::CharReaderBuilder rb;
        std::string err;
        std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
        if (body.empty() || !reader->parse(body.data(), body.data() + body.size(), &cfg, &err))
        {
            Json::Value e;
            e["status"] = "error";
            e["message"] = "invalid config json: " + err;
            client->response(jsonResponse(e));
            return;
        }
        // 写入配置并通知各驱动(中断基于旧配置的连接)
        setConfig(cfg);
    }
    Json::Value out;
    out["status"] = "ok";
    out["config"] = m_config;
    client->response(jsonResponse(out));
}

// 根页面: 转发到 :/simulator/index.html
void JZSimServer::handleHttpIndex(JZHttpServerClient* client)
{
    JZHttpRequest req;
    req.setPath("/index.html");
    m_static->handle(client, req);
}

// 具体驱动页面: /drivers/<cat> 或 /drivers/<cat>.html 映射到 :/simulator/drivers/<cat>.html
void JZSimServer::handleHttpDriverPage(JZHttpServerClient* client, const std::string& catPath)
{
    std::string category = catPath;
    // 去掉可能的 .html 后缀
    if (str_endsWith(category, ".html"))
        category = category.substr(0, category.size() - 5);
    // 防止路径穿越
    if (category.find('/') != std::string::npos || category.find('\\') != std::string::npos)
        category.clear();

    JZHttpRequest req;
    if (category.empty())
        req.setPath("/drivers/empty.html");
    else
        req.setPath("/drivers/" + category + ".html");
    m_static->handle(client, req);
}

// ===================== JSON 辅助 =====================
Json::Value JZSimServer::variantToJson(const JZVariant& v)
{
    Json::Value out;
    int tid = v.typeId();
    if (tid == JZMetaType::String)
        out = v.toString();
    else if (tid == JZMetaType::Int)
        out = v.toInt();
    else if (tid == JZMetaType::Bool)
        out = v.toBool();
    else if (tid == JZMetaType::Double)
        out = v.toDouble();
    else if (tid == JZMetaType::VariantList) {
        const JZVariantList& l = v.valueRef<JZVariantList>();
        out = Json::Value(Json::arrayValue);
        for (auto& e : l) out.append(variantToJson(e));
    } else if (tid == JZMetaType::VariantMap) {
        const JZVariantMap& m = v.valueRef<JZVariantMap>();
        out = variantMapToJson(m);
    } else {
        out = v.toString();
    }
    return out;
}

Json::Value JZSimServer::variantMapToJson(const JZVariantMap& m)
{
    Json::Value out(Json::objectValue);
    for (auto& kv : m)
        out[kv.first] = variantToJson(kv.second);
    return out;
}

static JZVariant jsonToVariant(const Json::Value& v)
{
    if (v.isString()) return JZVariant(v.asString());
    if (v.isBool()) return JZVariant(v.asBool());
    if (v.isInt()) return JZVariant(v.asInt());
    if (v.isUInt()) return JZVariant((int)v.asUInt());
    if (v.isDouble()) return JZVariant(v.asDouble());
    if (v.isArray()) {
        JZVariantList l; l.reserve(v.size());
        for (auto it = v.begin(); it != v.end(); ++it) l.push_back(jsonToVariant(*it));
        return JZVariant::fromValue(l);
    }
    if (v.isObject()) {
        JZVariantMap m;
        for (auto it = v.begin(); it != v.end(); ++it) m[it.name()] = jsonToVariant(*it);
        return JZVariant::fromValue(m);
    }
    return JZVariant();
}

Json::Value JZSimServer::variantEventsToJson(const std::vector<JZSimPack>& events)
{
    Json::Value arr(Json::arrayValue);
    for (auto& e : events) {
        Json::Value o;
        o["cmd"] = (int)e.cmd;
        o["data"] = JZSimServer::variantMapToJson(e.data);
        arr.append(o);
    }
    return arr;
}

static JZByteArray jsonResponse(const Json::Value& v)
{
    Json::FastWriter w;
    std::string s = w.write(v);
    JZHttpRespone resp;
    resp.setStatusCode(200);
    resp.setJson(s);
    return resp.toByteArray();
}
