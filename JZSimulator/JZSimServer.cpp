#include "JZSimServer.h"

#include "JZDataStream.h"
#include "JZByteArray.h"
#include "JZLog.h"
#include "JZDebug.h"
#include "JZJson.h"
#include "JZStd.h"

#include <algorithm>
#include <json/reader.h>
#include <json/writer.h>

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
        g_simInstance = new JZSimServer();
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
}

JZSimServer::~JZSimServer()
{
    stopHttp();
}

bool JZSimServer::startHttp(int port)
{
    if (m_http)
        return true;

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

    if (!m_http->listen(port))
    {
        jzDebug() << "JZSimServer: HTTP server listen failed on port" << port;
        delete m_http;
        m_http = nullptr;
        return false;
    }

    jzDebug() << "JZSimServer: HTTP server listening on port" << port;
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

// 收到"Device -> driver"事件时, 路由到各 driver simulator 的 onRecv。
void JZSimServer::onClientPacket(const JZSimPack& pack)
{
    for (auto it : m_recvMap)
    {
        JZSimulator* sim = it.second;
        if (sim && sim->support(pack.cmd))
            sim->onRecv(pack.cmd, pack.data);
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
    return root;
}

// ===================== HTTP 处理 =====================
void JZSimServer::handleHttpControl(JZHttpServerClient* client, const JZHttpRequest& req)
{
    Json::Value body; Json::Reader reader;
    if (!req.body().empty()) {
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
    out["events"] = variantEventsToJson();
    out["state"] = getStateJson();
    client->response(jsonResponse(out));
}

void JZSimServer::handleHttpPack(JZHttpServerClient* client, const JZHttpRequest& req)
{
    // 外部直接驱动 "Device -> driver" 事件(例如从网页触发应用侧收到 notify)
    Json::Value body; Json::Reader reader;
    if (req.body().empty() || !reader.parse(std::string(req.body().data(), req.body().size()), body)) {
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
    out["events"] = variantEventsToJson();
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

static Json::Value variantEventsToJson(const std::vector<JZSimPack>& events)
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
