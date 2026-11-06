#include "JZHubLink.h"

#include <algorithm>

#include "JZAgentHubPacket.h"
#include "JZDebug.h"
#include "JZNet/JZNetClient.h"
#include "JZNet/JZNetPacket.h"
#include "JZTimer.h"

using namespace std;

// 事件单包上限: 远低于 MG_MAX_PACK_SIZE(256KB), 留出包头与 JSON 键名的余量。
// 超了不硬切(切坏 JSON 会让 server 侧解析失败), 换成一个说明性的小对象。
static const size_t kMaxEventBytes = 96 * 1024;
static const size_t kMaxResultBytes = 160 * 1024;
static const size_t kPreviewBytes = 4 * 1024;

static string stripNewline(string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

// 按 UTF-8 边界往回退, 避免切出半个多字节字符
static string headBytes(const string &s, size_t n)
{
    if (s.size() <= n)
        return s;
    while (n > 0 && (s[n] & 0xC0) == 0x80)
        n--;
    return s.substr(0, n);
}

static string capPayload(const Json::Value &v, size_t maxBytes)
{
    string payload = stripNewline(Json::FastWriter().write(v));
    if (payload.size() <= maxBytes)
        return payload;

    Json::Value small(Json::objectValue);
    small["truncated"] = true;
    small["bytes"] = (Json::Int64)payload.size();
    small["preview"] = headBytes(payload, kPreviewBytes);
    return stripNewline(Json::FastWriter().write(small));
}

static bool parseJson(const string &text, Json::Value &out)
{
    Json::Reader reader;
    return reader.parse(text, out) && !out.isNull();
}

JZHubLink::JZHubLink(const Config &config, JZObject *parent)
    : JZObject(parent)
    , m_config(config)
    , m_agentId(config.agentId.empty() ? config.name : config.agentId)
{
}

JZHubLink::~JZHubLink()
{
    if (m_client)
    {
        m_client->disconnectFromHost();
        m_client->deleteLater();
        m_client = nullptr;
    }
}

void JZHubLink::connectToServer()
{
    if (m_client)
        return;

    m_client = new JZNetClient(this);
    if (!m_config.token.empty())
        m_client->setToken(m_config.token);

    connect(&m_client->sigConnected, this, &JZHubLink::onConnected);
    connect(&m_client->sigDisconnected, this, &JZHubLink::onDisconnected);
    connect(&m_client->sigPacketReceive, this, [this](JZNetPackPtr pack) { onPacket(pack); });

    jzInfo().noquote().nospace() << "连接 server(控制面) " << m_config.host << ":" << m_config.port
                                 << "  agentId=" << m_agentId;
    m_client->connectToHost(m_config.host, m_config.port);
}

void JZHubLink::onConnected()
{
    sendHello();

    // 与 JZNodeLink 同一个理由: JZNet 的 token 只派生密钥, 写错的表现是"包解不开",
    // 不给超时的话现场只会看到"连上了但什么也没发生"。
    JZTimer::singleShot(3000, this, [this]()
    {
        if (m_helloAcked)
            return;
        m_lastError = "server 没有回应 hello(3s): 多半是 token 不匹配或端口不对";
        jzWarning().noquote().nospace() << m_lastError;
        if (m_client)
            m_client->disconnectFromHost();
    });
}

void JZHubLink::onDisconnected()
{
    const bool wasOnline = m_online;
    m_online = false;
    m_helloAcked = false;
    m_acceptTasks = true;
    if (m_heartbeat)
        m_heartbeat->stop();

    if (wasOnline)
        jzWarning().noquote().nospace() << "与 server 的连接断开(任务仍在本地继续跑, 结束后再补发)";
    notifyState();
    scheduleReconnect();
}

void JZHubLink::onPacket(JZNetPackPtr pack)
{
    if (!pack)
        return;

    const int type = pack->packetType();
    if (type == HubPack_helloAck)
    {
        JZHubPackHelloAck *ack = (JZHubPackHelloAck *)pack.get();
        m_helloAcked = true;
        m_online = ack->ok;
        m_acceptTasks = ack->acceptTasks;
        m_serverTimeMs = ack->serverTimeMs;
        m_lastError = ack->ok ? string() : ack->error;
        m_retryMs = 1000;

        if (ack->ok)
        {
            jzInfo().noquote().nospace() << "server 已接受: agentId=" << m_agentId
                                         << (ack->acceptTasks ? "" : "  (维护模式: 不接任务)");
            if (!m_heartbeat)
            {
                m_heartbeat = new JZTimer(this);
                connect(&m_heartbeat->sigTimeout, this, [this]() { sendHeartbeat(); });
            }
            m_heartbeat->start(5000);
        }
        else
        {
            jzWarning().noquote().nospace() << "server 拒绝上线: " << ack->error;
        }
        notifyState();
        return;
    }

    if (type == HubPack_taskSubmit)
    {
        JZHubPackTaskSubmit *in = (JZHubPackTaskSubmit *)pack.get();
        Json::Value body;
        if (!parseJson(in->body, body) || !body.isObject())
        {
            jzWarning().noquote().nospace() << "taskSubmit 的 body 不是 JSON 对象, 已丢弃";
            Json::Value r;
            r["status"] = "failed";
            r["error"] = "taskSubmit body 不是合法 JSON";
            sendTaskEnd(in->taskId, r);
            return;
        }
        if (!m_acceptTasks)
        {
            Json::Value r;
            r["status"] = "failed";
            r["error"] = "server 处于维护模式, 不接受任务";
            sendTaskEnd(in->taskId, r);
            return;
        }
        jzInfo().noquote().nospace() << "收到任务: " << in->taskId;
        if (m_onSubmit)
            m_onSubmit(in->taskId, body);
        return;
    }

    if (type == HubPack_taskCancel)
    {
        JZHubPackTaskCancel *in = (JZHubPackTaskCancel *)pack.get();
        if (m_onCancel)
            m_onCancel(in->taskId);
        return;
    }

    if (type == HubPack_reset)
    {
        JZHubPackReset *in = (JZHubPackReset *)pack.get();
        if (m_onReset)
            m_onReset(in->scope);
        return;
    }
}

void JZHubLink::sendHello()
{
    if (!m_client)
        return;

    JZHubPackHello hello;
    hello.agentId = m_agentId;
    hello.name = m_config.name;
    hello.version = m_config.version;
    hello.rootDir = m_config.rootDir;
    hello.modelName = m_config.modelName;
    hello.nodes = nodesJson();
    m_client->sendPacket(&hello);
}

void JZHubLink::sendHeartbeat()
{
    if (!m_client || !m_online)
        return;

    JZHubPackHeartbeat hb;
    hb.busy = m_busy;
    hb.round = m_round;
    hb.taskId = m_taskId;
    hb.nodes = nodesJson();
    m_client->sendPacket(&hb);
}

void JZHubLink::scheduleReconnect()
{
    if (m_config.host.empty())
        return;

    const int delayMs = m_retryMs;
    m_retryMs = min(m_retryMs * 2, 30000);

    jzInfo().noquote().nospace() << "server 连接 " << delayMs << "ms 后重试";
    JZTimer::singleShot(delayMs, this, [this]()
    {
        if (m_client)
        {
            m_client->deleteLater();
            m_client = nullptr;
        }
        connectToServer();
    });
}

string JZHubLink::nodesJson() const
{
    return m_nodesProvider ? m_nodesProvider() : string("[]");
}

void JZHubLink::setTaskState(bool busy, int round, const string &taskId)
{
    m_busy = busy;
    m_round = round;
    m_taskId = taskId;
    // 状态变了立刻上报一次, 别等下一个 5s 心跳(管理台看到的忙闲才跟手)
    sendHeartbeat();
}

void JZHubLink::sendTaskEvent(const string &taskId, const string &type, const Json::Value &data)
{
    if (!m_client || !m_online || taskId.empty())
        return;

    JZHubPackTaskEvent ev;
    ev.taskId = taskId;
    ev.type = type;
    ev.data = capPayload(data, kMaxEventBytes);
    m_client->sendPacket(&ev);
}

void JZHubLink::sendTaskEnd(const string &taskId, const Json::Value &result)
{
    if (!m_client || !m_online)
    {
        jzWarning().noquote().nospace() << "与 server 断开, 任务 " << taskId << " 的回包没能送达";
        return;
    }

    JZHubPackTaskEnd end;
    end.taskId = taskId;
    end.result = capPayload(result, kMaxResultBytes);
    m_client->sendPacket(&end);
}

void JZHubLink::notifyState()
{
    if (m_onState)
        m_onState();
}
