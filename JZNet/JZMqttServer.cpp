#include "JZMqttServer.h"
#include "JZApplication.h"
#include "JZLog.h"
#include "JZDebug.h"

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

    return true;
}

void JZMqttServer::close()
{
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
    auto it = m_subcribeMap.begin();
    while (it != m_subcribeMap.end())
    {
        auto& subscribers = it->second;
        vec_removeAll(subscribers, client);

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

void JZMqttServer::onClinetNetEvent(JZMqttClient* client, JZNetResEvent* event)
{
    int ev = event->event;
    if (ev == JZ_NET_EV_MQTT_CMD)
    {
        //auto JZMgServerPacket *packet = new
        std::shared_ptr<JZMgServerPacket> resp = std::make_shared<JZMgServerPacket>();

        JZMqttResPacket* packet = (JZMqttResPacket*)(event->extData.get());
        auto* msg = &packet->message;
        auto cmd = msg->cmd;
        switch (cmd)
        {
            case MQTT_CMD_CONNECT:
            {
                uint8_t rc = 0;
                resp->cmd = MQTT_CMD_CONNACK;
                resp->flags = 0;
                resp->len = 2;
                resp->buffer = {0x00, rc};
                break;
            }
            case MQTT_CMD_PUBLISH:
            {
                uint8_t qos = msg->qos;
                if (qos == 1)
                {
                    resp->cmd = MQTT_CMD_PUBACK;
                    resp->flags = 0;
                    resp->len = 2;
                    resp->buffer = {(uint8_t)(msg->id >> 8), (uint8_t)msg->id};
                }
                else if (qos == 2)
                {
                    resp->cmd = MQTT_CMD_PUBREC;
                    resp->flags = 0;
                    resp->len = 2;
                    resp->buffer = {(uint8_t)(msg->id >> 8), (uint8_t)msg->id};
                }
                break;
            }
            case MQTT_CMD_PUBREC:
            {
                resp->cmd = MQTT_CMD_PUBREL;
                resp->flags = 0x02;
                resp->len = 2;
                resp->buffer = {(uint8_t)(msg->id >> 8), (uint8_t)msg->id};
                break;
            }
            case MQTT_CMD_PUBREL:
            {
                resp->cmd = MQTT_CMD_PUBCOMP;
                resp->flags = 0;
                resp->len = 2;
                resp->buffer = {(uint8_t)(msg->id >> 8), (uint8_t)msg->id};
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
                        if (!vec_contains(subscribers, client))
                        {
                            subscribers.push_back(client);
                        }
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
                            vec_removeAll(subscribers, client);

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
            default:
                break;
        }

        JZNetReqEvent* req = new JZNetReqEvent();
        req->netCmd = JZNetReqEvent::Net_Write;
        req->writeCmd = WriteMqttServerResp;
        req->conn = client->m_client;
        req->extData = resp;
        postReqEvent(req);
    }
    else if (ev == JZ_NET_EV_MQTT_MSG)
    {
        JZMqttResPacket* packet = (JZMqttResPacket*)(event->extData.get());
        auto* msg = &packet->message;
        auto it = m_subcribeMap.find(msg->topic);
        if (it != m_subcribeMap.end())
        {
            auto& subscribers = it->second;
            for (int i = 0; i < (int)subscribers.size(); i++)
            {
                JZMqttPublishOpt opt;
                opt.topic = msg->topic;
                opt.payload = msg->payload;
                opt.props = msg->props; 

                subscribers[i]->publish(opt);
            }
        }
    }
}