#ifndef JZ_NET_MQTT_PACKET_H_
#define JZ_NET_MQTT_PACKET_H_
#include <string>
#include <vector>
#include <memory>
#include "JZByteArray.h"
#include "JZNetObject.h"

struct mg_mqtt_opts;
class JZMgOptPacketBuilder;
class JZFRAMEWORK_API JZMgOptPacket : public JZNetReqEventData
{
public:
    JZMgOptPacket();
    ~JZMgOptPacket();

    enum MqttCommand
    {
        None,
        Connect,
        Publish,
        Subscribe,
        Unsubscribe,
    };

    MqttCommand command = None;
    mg_mqtt_opts *opt;

protected:
    friend JZMgOptPacketBuilder;

    JZ_DISABLE_COPY(JZMgOptPacket)

    size_t m_offset = 0;
    std::vector<char> m_buffer;  // 统一存放malloc风格字符串+mg_mqtt_prop数组
};
typedef std::shared_ptr<JZMgOptPacket> JZMgOptPacketPtr;

// MQTT5通用属性
struct JZFRAMEWORK_API JZMqttProp
{
    uint8_t propId{};
    uint32_t intVal{};
    std::string key, val;
};

// ① 连接配置：CONNECT报文专用
struct JZFRAMEWORK_API JZMqttConnectOpt
{
    std::string clientId;
    std::string userName, password;
    uint16_t keepAlive = 60;
    uint8_t protoVer = 4;
    bool cleanSession = true;

    // 遗嘱参数
    std::string willTopic, willMsg;
    uint8_t willQos = 0;
    bool willRetain = false;
    std::vector<JZMqttProp> willProps;
};

// ② 发布配置：PUBLISH专用
struct JZFRAMEWORK_API JZMqttPublishOpt
{
    std::string topic;
    JZByteArray payload;   // 消息负载(可为二进制)
    uint8_t qos = 0;
    bool retain = false;
    int retransmitId = 0;
    std::vector<JZMqttProp> props;
};

// ③ 订阅配置：SUBSCRIBE/UNSUBSCRIBE专用
struct JZFRAMEWORK_API JZMqttSubOpt
{
    std::string topic;
    uint8_t qos = 0;
    std::vector<JZMqttProp> props;
};

JZMgOptPacketPtr makeMgOptPacket(const JZMqttConnectOpt& src);
JZMgOptPacketPtr makeMgOptPacket(const JZMqttPublishOpt& src);
JZMgOptPacketPtr makeMgOptPacket(bool is_sub, const JZMqttSubOpt& src);

// JZMqttMessage
class JZFRAMEWORK_API JZMqttMessage
{
public:
    std::string topic;
    JZByteArray payload;   // 消息负载(可为二进制)
    JZByteArray rawPacket;

    uint16_t id{};
    uint8_t cmd{};
    uint8_t qos{};
    uint8_t ackCode{};

    // MQTT5属性
    std::vector<JZMqttProp> props;
};

// 从原生mg_mqtt_message解析(收消息)
struct mg_mqtt_message;
void mgMsgToMqttMessage(struct mg_mqtt_message* mg_msg, JZMqttMessage *msg);

class JZFRAMEWORK_API JZMgServerPacket : public JZNetReqEventData
{
public:
    uint8_t cmd;
    uint8_t flags;
    uint32_t len;
    std::vector<uint8_t> buffer;
};

//JZMqttResPacket
class JZFRAMEWORK_API JZMqttResPacket : public JZNetResEventData
{
public:
    ~JZMqttResPacket();
    
    JZMqttMessage message;
};

#endif