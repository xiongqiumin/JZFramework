#include "JZMqttPacket.h"
#include "JZMqttPacket_p.h"

//JZMgOptPacket
JZMgOptPacket::JZMgOptPacket()
{
    m_buffer.resize(1024);
}

JZMgOptPacket::~JZMgOptPacket()
{
}

// 从buffer里分配内存
// 内部：字符串拷贝进buffer，返回mg_str
mg_str JZMgOptPacket::buildStr(const std::string& s)
{
    if (s.empty())
        return mg_str{nullptr, 0};

    JZ_ASSERT(m_offset + s.size() + 1 <= m_buffer.size());

    size_t pos = m_offset;
    memcpy(&m_buffer[m_offset], s.data(), s.size());
    m_offset += s.size();
    m_buffer[m_offset++] = '\0';

    return mg_str_n(&m_buffer[pos], s.size());
}

// 内部：单个属性存入buffer
mg_mqtt_prop JZMgOptPacket::buildProp(const JZMqttProp& src)
{
    mg_mqtt_prop p{};
    p.id = src.propId;
    p.iv = src.intVal;
    p.key = buildStr(src.key);
    p.val = buildStr(src.val);
    return p;
}

// 属性数组批量申请buffer空间
mg_mqtt_prop* JZMgOptPacket::allocPropArr(size_t count)
{
    if (count == 0)
        return nullptr;

    size_t pos = m_offset;
    m_offset += (pos + count * sizeof(mg_mqtt_prop));
    JZ_ASSERT(m_offset <= m_buffer.size());

    return reinterpret_cast<mg_mqtt_prop*>(&m_buffer[pos]);
}

JZMgOptPacketPtr makeMgOptPacket(const JZMqttConnectOpt& src)
{
    auto pkt = std::make_shared<JZMgOptPacket>();
    pkt->command = JZMgOptPacket::Connect;
    auto& opt = pkt->opt;

     // client/user/pass全部落地buffer
    opt.client_id = pkt->buildStr(src.clientId);
    opt.user = pkt->buildStr(src.userName);
    opt.pass = pkt->buildStr(src.password);
    opt.keepalive = src.keepAlive;
    opt.version = src.protoVer;
    opt.clean = src.cleanSession;

    size_t wCnt = src.willProps.size();
    opt.will_props = pkt->allocPropArr(wCnt);
    opt.num_will_props = wCnt;
    for (size_t i = 0; i < wCnt; ++i)
        opt.will_props[i] = pkt->buildProp(src.willProps[i]);

    opt.props = nullptr;
    opt.num_props = 0;
    return pkt;
}

// pub/sub封装
JZMgOptPacketPtr makeMgOptPacket(const JZMqttPublishOpt& src)
{
    auto pkt = std::make_shared<JZMgOptPacket>();
    pkt->command = JZMgOptPacket::Publish;
    auto& opt = pkt->opt;

    opt.topic = pkt->buildStr(src.topic);
    opt.message = pkt->buildStr(src.payload);
    opt.qos = src.qos;
    opt.retain = src.retain;
    opt.retransmit_id = src.retransmitId;
    opt.will_props = nullptr;
    opt.num_will_props = 0;

    size_t pCnt = src.props.size();
    opt.props = pkt->allocPropArr(pCnt);
    opt.num_props = pCnt;
    for (size_t i = 0; i < pCnt; ++i)
        opt.props[i] = pkt->buildProp(src.props[i]);

    return pkt;
}

JZMgOptPacketPtr makeMgOptPacket(bool is_sub, const JZMqttSubOpt& src)
{
    auto pkt = std::make_shared<JZMgOptPacket>();
    pkt->command = is_sub? JZMgOptPacket::Subscribe : JZMgOptPacket::Unsubscribe;
    auto& opt = pkt->opt;

    opt.topic = pkt->buildStr(src.topic);
    opt.qos = src.qos;
    opt.will_props = nullptr;
    opt.num_will_props = 0;

    size_t pCnt = src.props.size();
    opt.props = pkt->allocPropArr(pCnt);
    opt.num_props = pCnt;
    for (size_t i = 0; i < pCnt; ++i)
        opt.props[i] = pkt->buildProp(src.props[i]);

    return pkt;
}

//JZMqttMessage
void mgMsgToMqttMessage(struct mg_mqtt_message* msg, JZMqttMessage* mqtt_msg)
{
    mqtt_msg->topic.assign(msg->topic.buf, msg->topic.len);
    mqtt_msg->payload.assign(msg->data.buf, msg->data.len);
    mqtt_msg->rawPacket = JZByteArray(msg->dgram.buf, msg->dgram.len);

    mqtt_msg->id = msg->id;
    mqtt_msg->cmd = msg->cmd;
    mqtt_msg->qos = msg->qos;
    mqtt_msg->ackCode = msg->ack;

    mqtt_msg->props.clear();

    size_t off = 0;
    mg_mqtt_prop mp{};
    while ((off = mg_mqtt_next_prop(msg, &mp, off)) != 0)
    {
        JZMqttProp p{};
        p.propId = mp.id;
        p.intVal = mp.iv;
        p.key.assign(mp.key.buf, mp.key.len);
        p.val.assign(mp.val.buf, mp.val.len);
        mqtt_msg->props.push_back(p);
    }
}

//JZMqttResPacket
JZMqttResPacket::~JZMqttResPacket()
{
}