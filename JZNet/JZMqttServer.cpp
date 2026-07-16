#include "JZMqttServer.h"
#include "JZApplication.h"
#include "JZLog.h"
#include "JZDebug.h"
#include "JZPlatform.h"
#include "JZTimer.h"
#include <algorithm>

#define MQTT_CMD_CONNECT 1
#define MQTT_CMD_CONNACK 2
#define MQTT_CMD_PUBLISH 3
#define MQTT_CMD_PUBACK 4
#define MQTT_CMD_PUBREC 5
#define MQTT_CMD_PUBREL 6
#define MQTT_CMD_PUBCOMP 7
#define MQTT_CMD_SUBSCRIBE 8
#define MQTT_CMD_SUBACK 9
#define MQTT_CMD_UNSUBSCRIBE 10
#define MQTT_CMD_UNSUBACK 11
#define MQTT_CMD_PINGREQ 12
#define MQTT_CMD_PINGRESP 13
#define MQTT_CMD_DISCONNECT 14
#define MQTT_CMD_AUTH 15

using namespace std;

JZMqttServer::JZMqttServer(JZObject* parent) 
    :JZNetObject(parent), sigAccept(this)
{
    m_listen = INVALID_NET_HANDLE;
    m_keepaliveTimer = new JZTimer(this);
    connect(&m_keepaliveTimer->sigTimeout, this, &JZMqttServer::onKeepaliveTimer);
}

JZMqttServer::~JZMqttServer()
{
    close();
}

bool JZMqttServer::isListening() const
{
    return m_listen;
}

bool JZMqttServer::listen(int port)
{
    char ip_addr[32];
    sprintf(ip_addr, "0.0.0.0:%d", port);

    JZSocketInfo info;
    info.url = ip_addr;
    info.type = JZSocketInfo::MqttServer;
    m_listen = openConn(info);
    if (!m_listen)
        return false;

    m_keepaliveTimer->start(1000);   // 每秒扫描一次心跳超时
    return true;
}

void JZMqttServer::close()
{
    if (m_keepaliveTimer)
        m_keepaliveTimer->stop();
    m_clientState.clear();

    if (m_listen)
    {
        closeConn(m_listen, false);
        m_listen = INVALID_NET_HANDLE;
    }
}

JZNetObject* JZMqttServer::onAccept(uint64_t conn)
{
    JZMqttClient* socket = new JZMqttClient(this);
    socket->m_client = conn;
    socket->m_state = ConnectedState;
    socket->m_server = this;
    socket->moveToThread(this->thread());
    connect(&socket->sigDisconnect, this, &JZMqttServer::onClientDisconnect);

    JZObjectPointer socket_ptr = socket->guardPointer();
    auto notify = [this, socket_ptr]
    {
        if (socket_ptr)
        {
            JZMqttClient* socket = (JZMqttClient*)socket_ptr.object();
            m_pendingConnections.push_back(socket);
            this->sigAccept.emit();
        }
    };

    runOnThread(this->thread(), this, notify);
    return socket;
}

JZMqttClient* JZMqttServer::nextPendingConnection()
{
    if (m_pendingConnections.empty())
        return nullptr;

    JZMqttClient* socket = m_pendingConnections.front();
    m_pendingConnections.pop_front();
    return socket;
}

void JZMqttServer::onNetEvent(JZNetResEvent* event)
{
}

void JZMqttServer::onClientDisconnect()
{
    auto client = (JZMqttClient*) sender();

    // 异常断开: 发布遗嘱(LWT)
    auto sit = m_clientState.find(client);
    if (sit != m_clientState.end())
    {
        if (sit->second.hasWill)
        {
            const JZMqttPublishOpt& will = sit->second.will;
            for (auto& kv : m_subcribeMap)
            {
                if (!topicMatch(kv.first, will.topic))
                    continue;
                for (auto& entry : kv.second)
                {
                    JZMqttPublishOpt opt = will;
                    opt.qos = std::min<uint8_t>(will.qos, entry.qos);
                    entry.client->publish(opt);
                }
            }
        }
        m_clientState.erase(sit);
    }

    // 清理该客户端的全部订阅
    for (auto it = m_subcribeMap.begin(); it != m_subcribeMap.end(); )
    {
        auto& subscribers = it->second;
        for (size_t i = 0; i < subscribers.size(); )
        {
            if (subscribers[i].client == client)
                subscribers.erase(subscribers.begin() + i);
            else
                i++;
        }

        if (subscribers.empty())
            it = m_subcribeMap.erase(it);
        else
            it++;
    }
}

JZOptional<JZMqttServer::SubscribePacket> JZMqttServer::parseSubscribeMessage(char* data, int data_size)
{
    SubscribePacket packet;
    size_t pos = 0;

    if (data_size < 3)
        return JZOptional<SubscribePacket>::None();

    // 1. 解析固定头
    uint8_t cmd = data[pos] & 0xF0;
    uint8_t flags = data[pos] & 0x0F;

    // SUBSCRIBE (0x80) 或 UNSUBSCRIBE (0xA0)
    if ((cmd != 0x80 && cmd != 0xA0) || flags != 0x02)
        return JZOptional<SubscribePacket>::None();

    bool isSubscribe = (cmd == 0x80);
    pos++;

    // 2. 解析剩余长度
    int remainingLen = 0;
    int multiplier = 1;
    while (pos < data_size && (data[pos] & 0x80))
    {
        remainingLen += (data[pos] & 0x7F) * multiplier;
        multiplier *= 128;
        pos++;
        if (multiplier > 128 * 128 * 128)
            return JZOptional<SubscribePacket>::None();
    }
    if (pos < data_size)
    {
        remainingLen += data[pos] * multiplier;
        pos++;
    }

    // 3. 解析包ID
    if (pos + 2 > data_size)
        return JZOptional<SubscribePacket>::None();
    packet.packetId = (uint8_t(data[pos]) << 8) | uint8_t(data[pos + 1]);
    pos += 2;

    // 4. 解析主题列表
    while (pos < data_size)
    {
        if (pos + 2 > data_size)
            return JZOptional<SubscribePacket>::None();
        uint16_t topicLen = (uint8_t(data[pos]) << 8) | uint8_t(data[pos + 1]);
        pos += 2;

        if (pos + topicLen > data_size)
            return JZOptional<SubscribePacket>::None();

        if (topicLen == 0)
            return JZOptional<SubscribePacket>::None();

        SubscribeTopic topic;
        topic.topic.assign(&data[pos], topicLen);
        pos += topicLen;

        if (isSubscribe)
        {
            // SUBSCRIBE: 有 QoS 字节
            if (pos >= data_size)
                return JZOptional<SubscribePacket>::None();
            topic.qos = uint8_t(data[pos++]);
            if (topic.qos > 2)  // QoS 只能是 0-2
                return JZOptional<SubscribePacket>::None();
        }
        else
        {
            // UNSUBSCRIBE: 没有 QoS 字节，设为 0（无意义，仅为兼容结构）
            topic.qos = 0;
        }

        packet.topics.push_back(std::move(topic));
    }

    return packet;
}

static std::vector<std::string> mqttSplitLevels(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s)
    {
        if (c == '/') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

bool JZMqttServer::topicMatch(const std::string& filter, const std::string& topic)
{
    auto f = mqttSplitLevels(filter);
    auto t = mqttSplitLevels(topic);
    for (size_t i = 0; i < f.size(); i++)
    {
        if (f[i] == "#")
            return true;                       // 匹配剩余所有层级(含零层)
        if (i >= t.size())
            return false;
        if (f[i] == "+")
            continue;                          // 匹配单个层级
        if (f[i] != t[i])
            return false;
    }
    return f.size() == t.size();
}

void JZMqttServer::deliverRetained(JZMqttClient* client, const std::string& filter, uint8_t subQos)
{
    for (auto& kv : m_retainedMap)
    {
        if (!topicMatch(filter, kv.first))
            continue;
        JZMqttPublishOpt opt = kv.second;
        opt.qos = std::min<uint8_t>(opt.qos, subQos);
        opt.retain = true;                     // 投递给新订阅者时置 retain 标志
        client->publish(opt);
    }
}

JZOptional<JZMqttServer::ClientConnState> JZMqttServer::parseConnect(const JZByteArray& dgram)
{
    const uint8_t* p = (const uint8_t*)dgram.data();
    int len = dgram.size();
    int pos = 0;
    if (len < 2 || (p[pos] & 0xF0) != 0x10)
        return JZOptional<ClientConnState>::None();   // 非 CONNECT
    pos++;

    // 跳过剩余长度(varint)
    while (pos < len && (p[pos] & 0x80)) pos++;
    if (pos >= len) return JZOptional<ClientConnState>::None();
    pos++;

    // 协议名 "MQTT"
    if (pos + 2 > len) return JZOptional<ClientConnState>::None();
    uint16_t nameLen = (uint16_t(p[pos]) << 8) | p[pos + 1];
    pos += 2;
    if (pos + nameLen > len) return JZOptional<ClientConnState>::None();
    std::string name((const char*)p + pos, nameLen);
    pos += nameLen;
    if (name != "MQTT")
        return JZOptional<ClientConnState>::None();

    // 协议级别(3/4/5)
    if (pos + 1 > len) return JZOptional<ClientConnState>::None();
    uint8_t level = p[pos++];
    if (level != 3 && level != 4 && level != 5)
        return JZOptional<ClientConnState>::None();

    // 连接标志
    if (pos + 1 > len) return JZOptional<ClientConnState>::None();
    uint8_t flags = p[pos++];
    bool willRetain = flags & 0x20;
    uint8_t willQos = (flags >> 3) & 0x03;
    bool willFlag = flags & 0x04;

    // Keep Alive
    if (pos + 2 > len) return JZOptional<ClientConnState>::None();
    ClientConnState st;
    st.keepAlive = (uint16_t(p[pos]) << 8) | p[pos + 1];
    pos += 2;

    auto readStr = [&](std::string& out) -> bool {
        if (pos + 2 > len) return false;
        uint16_t sl = (uint16_t(p[pos]) << 8) | p[pos + 1];
        pos += 2;
        if (pos + sl > len) return false;
        out.assign((const char*)p + pos, sl);
        pos += sl;
        return true;
    };

    std::string clientId;
    if (!readStr(clientId)) return JZOptional<ClientConnState>::None();
    if (willFlag)
    {
        std::string wt, wm;
        if (!readStr(wt) || !readStr(wm)) return JZOptional<ClientConnState>::None();
        st.hasWill = true;
        st.will.topic = wt;
        st.will.payload = wm;
        st.will.qos = willQos;
        st.will.retain = willRetain;
    }
    if (flags & 0x80) { std::string s; if (!readStr(s)) return JZOptional<ClientConnState>::None(); }  // user
    if (flags & 0x40) { std::string s; if (!readStr(s)) return JZOptional<ClientConnState>::None(); }  // pass

    return st;
}

void JZMqttServer::onKeepaliveTimer()
{
    uint64_t now = (uint64_t)JZPlatform::getTickCountMs();
    for (auto it = m_clientState.begin(); it != m_clientState.end(); )
    {
        auto& st = it->second;
        if (st.keepAlive > 0 && now - st.lastActiveMs > (uint64_t)st.keepAlive * 1500)
        {
            JZMqttClient* c = it->first;
            it++;
            if (c->m_client != INVALID_NET_HANDLE)
                closeConn(c->m_client, true);   // 心跳超时: 视为异常断开, 触发遗嘱
        }
        else
        {
            it++;
        }
    }
}

void JZMqttServer::onClinetNetEvent(JZMqttClient* client, JZNetResEvent* event)
{
    int ev = event->event;

    // 任意 MQTT 报文都刷新心跳计时
    auto sit = m_clientState.find(client);
    if (sit != m_clientState.end())
        sit->second.lastActiveMs = JZPlatform::getTickCountMs();

    if (ev == JZ_NET_EV_CLOSE)
    {
        if (client->m_client != INVALID_NET_HANDLE)
        {
            client->m_client = INVALID_NET_HANDLE;
            client->closeInternal(false);   // 触发 sigDisconnect → 清理订阅 + 遗嘱
        }
        return;
    }

    if (ev == JZ_NET_EV_MQTT_CMD)
    {
        std::shared_ptr<JZMgServerPacket> resp = std::make_shared<JZMgServerPacket>();

        JZMqttResPacket* packet = (JZMqttResPacket*)(event->extData.get());
        auto* msg = &packet->message;
        auto cmd = msg->cmd;
        bool needClose = false;
        bool noResponse = false;
        switch (cmd)
        {
            case MQTT_CMD_CONNECT:
            {
                uint8_t rc = 0;
                auto opt = parseConnect(msg->rawPacket);
                if (!opt.isSome())
                    rc = 1;                 // 不可接受的协议名/版本
                else
                    m_clientState[client] = opt.value();

                resp->cmd = MQTT_CMD_CONNACK;
                resp->flags = 0;
                resp->len = 2;
                resp->buffer = {0x00, rc};

                if (rc != 0)
                    needClose = true;       // 协议错误: 回 CONNACK 后断开
                break;
            }
            case MQTT_CMD_PUBLISH:
            {
                // 入站 QoS 确认(PUBACK/PUBREC)由 mongoose 自动回
                // 消息转发在 JZ_NET_EV_MQTT_MSG 分支处理, 此处无需回包
                noResponse = true;
                break;
            }
            case MQTT_CMD_PUBREC:
            {
                // 入站 PUBREC → mongoose 已自动回 PUBREL, 无需回包
                noResponse = true;
                break;
            }
            case MQTT_CMD_PUBREL:
            {
                // 入站 PUBREL → mongoose 已自动回 PUBCOMP, 无需回包
                noResponse = true;
                break;
            }
            case MQTT_CMD_SUBSCRIBE:
            {
                auto pid = msg->id;
                resp->cmd = MQTT_CMD_SUBACK;
                resp->flags = 0;
                resp->len = 3;
                resp->buffer = {(uint8_t)(pid >> 8), (uint8_t)pid, 0x00};

                JZOptional<JZMqttServer::SubscribePacket> opt =
                    JZMqttServer::parseSubscribeMessage(msg->rawPacket.data(), msg->rawPacket.size());
                
                if (opt.isSome())
                {
                    auto &subcribe = opt.value();
                    for (auto& top : subcribe.topics)
                    {
                        auto& subscribers = m_subcribeMap[top.topic];
                        bool found = false;
                        for (auto& e : subscribers)
                        {
                            if (e.client == client) { e.qos = top.qos; found = true; break; }
                        }
                        if (!found)
                            subscribers.push_back({client, top.qos});

                        deliverRetained(client, top.topic, top.qos);
                    }
                }
                else
                {
                    jzWarning() << "parse MQTT_CMD_SUBSCRIBE failed";
                    client->deleteLater();
                }
                break;
            }
            case MQTT_CMD_UNSUBSCRIBE:
            {
                auto pid = msg->id;
                resp->cmd = MQTT_CMD_UNSUBACK;
                resp->flags = 0;
                resp->len = 2;
                resp->buffer = {(uint8_t)(pid >> 8), (uint8_t)pid};

                JZOptional<JZMqttServer::SubscribePacket> opt =
                    JZMqttServer::parseSubscribeMessage(msg->rawPacket.data(), msg->rawPacket.size());
                
                if (opt.isSome())
                {
                    auto& subcribe = opt.value();
                    for (auto& top : subcribe.topics)
                    {
                        auto it = m_subcribeMap.find(top.topic);
                        if (it != m_subcribeMap.end())
                        {
                            auto& subscribers = it->second;
                            for (size_t i = 0; i < subscribers.size(); )
                            {
                                if (subscribers[i].client == client)
                                    subscribers.erase(subscribers.begin() + i);
                                else
                                    i++;
                            }

                            if (subscribers.empty())
                                m_subcribeMap.erase(it);
                        }
                    }
                }
                else
                {
                    jzWarning() << "parse MQTT_CMD_UNSUBSCRIBE failed";
                    client->deleteLater();
                }
                break;
            }
            case MQTT_CMD_PINGREQ:
            {
                resp->cmd = MQTT_CMD_PINGRESP;
                resp->flags = 0;
                resp->len = 0;
                break;
            }
            case MQTT_CMD_DISCONNECT:
            {
                // 优雅断开: 不发布遗嘱
                auto dit = m_clientState.find(client);
                if (dit != m_clientState.end())
                    dit->second.hasWill = false;
                break;
            }
            case MQTT_CMD_PUBACK:
            case MQTT_CMD_PUBCOMP:
            {
                // 订阅者对 broker 出站 QoS 消息的确认: 清除重传跟踪, 无需回包
                client->onPubAck(msg->id);
                noResponse = true;
                break;
            }
            default:
                break;
        }

        if (!noResponse)
        {
            JZNetReqEvent* req = new JZNetReqEvent();
            req->netCmd = JZNetReqEvent::Net_Write;
            req->writeCmd = WriteMqttServerResp;
            req->conn = client->m_client;
            req->extData = resp;
            postReqEvent(req);
        }

        if (needClose)
            closeConn(client->m_client, true);
    }
    else if (ev == JZ_NET_EV_MQTT_MSG)
    {
        JZMqttResPacket* packet = (JZMqttResPacket*)(event->extData.get());
        auto* msg = &packet->message;

        // 处理 retain: 缓存最新保留消息(空 payload 表示清除)
        bool retain = (msg->rawPacket.size() > 0) && (msg->rawPacket[0] & 0x01);
        if (retain)
        {
            JZMqttPublishOpt ropt;
            ropt.topic = msg->topic;
            ropt.payload = msg->payload;
            ropt.props = msg->props;
            ropt.qos = msg->qos;
            if (ropt.payload.empty())
                m_retainedMap.erase(msg->topic);
            else
                m_retainedMap[msg->topic] = ropt;
        }

        // 转发给所有匹配的订阅者, 生效 QoS = min(发布QoS, 订阅QoS)
        for (auto& kv : m_subcribeMap)
        {
            if (!topicMatch(kv.first, msg->topic))
                continue;

            for (auto& entry : kv.second)
            {
                JZMqttPublishOpt opt;
                opt.topic = msg->topic;
                opt.payload = msg->payload;
                opt.props = msg->props;
                opt.qos = std::min<uint8_t>(msg->qos, entry.qos);
                entry.client->publish(opt);
            }
        }
    }
}