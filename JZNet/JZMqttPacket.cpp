#include "JZMqttPacket.h"
#include "mongoose.h"

// JZMgOptPacket
JZMgOptPacket::JZMgOptPacket()
{
    opt = new mg_mqtt_opts();
    m_buffer.resize(1024);
}

JZMgOptPacket::~JZMgOptPacket()
{
    delete opt;
}

//JZMgOptPacketBuilder
class JZMgOptPacketBuilder
{
public:
    JZMgOptPacketBuilder(JZMgOptPacket* pkt);

    mg_str buildStr(const std::string &text);
    mg_mqtt_prop* allocPropArr(size_t count);
    mg_mqtt_prop buildProp(const JZMqttProp &sp);

protected:
    JZMgOptPacket* d;
};

JZMgOptPacketBuilder::JZMgOptPacketBuilder(JZMgOptPacket* pkt)
{
    d = pkt;
}

// 从buffer里分配内存
// 内部：字符串拷贝进buffer，返回mg_str
mg_str JZMgOptPacketBuilder::buildStr(const std::string& s)
{
    if (s.empty())
        return mg_str{nullptr, 0};

    JZ_ASSERT(d->m_offset + s.size() + 1 <= d->m_buffer.size());

    size_t pos = d->m_offset;
    memcpy(&d->m_buffer[d->m_offset], s.data(), s.size());
    d->m_offset += s.size();
    d->m_buffer[d->m_offset++] = '\0';

    return mg_str_n(&d->m_buffer[pos], s.size());
}

// 内部：单个属性存入buffer
mg_mqtt_prop JZMgOptPacketBuilder::buildProp(const JZMqttProp& src)
{
    mg_mqtt_prop p{};
    p.id = src.propId;
    p.iv = src.intVal;
    p.key = buildStr(src.key);
    p.val = buildStr(src.val);
    return p;
}

// 属性数组批量申请buffer空间
mg_mqtt_prop* JZMgOptPacketBuilder::allocPropArr(size_t count)
{
    if (count == 0)
        return nullptr;

    size_t pos = d->m_offset;
    d->m_offset += (pos + count * sizeof(mg_mqtt_prop));
    JZ_ASSERT(d->m_offset <= d->m_buffer.size());

    return reinterpret_cast<mg_mqtt_prop*>(&d->m_buffer[pos]);
}

JZMgOptPacketPtr makeMgOptPacket(const JZMqttConnectOpt& src)
{
    auto pkt = std::make_shared<JZMgOptPacket>();
    pkt->command = JZMgOptPacket::Connect;
    auto& opt = *pkt->opt;

    JZMgOptPacketBuilder builder(pkt.get());

     // client/user/pass全部落地buffer
    opt.client_id = builder.buildStr(src.clientId);
    opt.user = builder.buildStr(src.userName);
    opt.pass = builder.buildStr(src.password);
    opt.keepalive = src.keepAlive;
    opt.version = src.protoVer;
    opt.clean = src.cleanSession;

    size_t wCnt = src.willProps.size();
    opt.will_props = builder.allocPropArr(wCnt);
    opt.num_will_props = wCnt;
    for (size_t i = 0; i < wCnt; ++i)
        opt.will_props[i] = builder.buildProp(src.willProps[i]);

    opt.props = nullptr;
    opt.num_props = 0;
    return pkt;
}

// pub/sub封装
JZMgOptPacketPtr makeMgOptPacket(const JZMqttPublishOpt& src)
{
    auto pkt = std::make_shared<JZMgOptPacket>();
    pkt->command = JZMgOptPacket::Publish;
    auto& opt = *pkt->opt;

    JZMgOptPacketBuilder builder(pkt.get());

    opt.topic = builder.buildStr(src.topic);
    opt.message = builder.buildStr(src.payload);
    opt.qos = src.qos;
    opt.retain = src.retain;
    opt.retransmit_id = src.retransmitId;
    opt.will_props = nullptr;
    opt.num_will_props = 0;

    size_t pCnt = src.props.size();
    opt.props = builder.allocPropArr(pCnt);
    opt.num_props = pCnt;
    for (size_t i = 0; i < pCnt; ++i)
        opt.props[i] = builder.buildProp(src.props[i]);

    return pkt;
}

JZMgOptPacketPtr makeMgOptPacket(bool is_sub, const JZMqttSubOpt& src)
{
    auto pkt = std::make_shared<JZMgOptPacket>();
    pkt->command = is_sub ? JZMgOptPacket::Subscribe : JZMgOptPacket::Unsubscribe;
    auto& opt = *pkt->opt;

    JZMgOptPacketBuilder builder(pkt.get());

    opt.topic = builder.buildStr(src.topic);
    opt.qos = src.qos;
    opt.will_props = nullptr;
    opt.num_will_props = 0;

    size_t pCnt = src.props.size();
    opt.props = builder.allocPropArr(pCnt);
    opt.num_props = pCnt;
    for (size_t i = 0; i < pCnt; ++i)
        opt.props[i] = builder.buildProp(src.props[i]);

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