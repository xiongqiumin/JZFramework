#ifndef JZ_MQTT_PACKET_H_
#define JZ_MQTT_PACKET_H_

#include <memory>
#include <vector>
#include "mongoose.h"
#include "JZNetObject.h"
#include "JZMqttPacket.h"

class JZMgOptPacket : public JZNetReqEventData
{
public:
    enum MqttCommand
    {
        None,
        Connect,
        Publish,
        Subscribe,
        Unsubscribe,
    };

    JZMgOptPacket();
	~JZMgOptPacket();

    mg_str buildStr(const std::string &text);
    mg_mqtt_prop* allocPropArr(size_t count);
    mg_mqtt_prop buildProp(const JZMqttProp &sp);
	
	MqttCommand command = None;
    mg_mqtt_opts opt{};

protected:
    size_t m_offset = 0;
    std::vector<char> m_buffer;  // 统一存放malloc风格字符串+mg_mqtt_prop数组

};
typedef std::shared_ptr<JZMgOptPacket> JZMgOptPacketPtr;

class JZMgOptPacket;
std::shared_ptr<JZMgOptPacket> makeMgOptPacket(const JZMqttConnectOpt& src);
std::shared_ptr<JZMgOptPacket> makeMgOptPacket(const JZMqttPublishOpt& src);
std::shared_ptr<JZMgOptPacket> makeMgOptPacket(bool is_sub, const JZMqttSubOpt& src);

class JZMgServerPacket : public JZNetReqEventData
{
public:
    uint8_t cmd;
    uint8_t flags;
    uint32_t len;
    std::vector<uint8_t> buffer;
};

// 从原生mg_mqtt_message解析(收消息)
void mgMsgToMqttMessage(struct mg_mqtt_message* mg_msg, JZMqttMessage *msg);

//JZMqttResPacket
class JZMqttResPacket : public JZNetResEventData
{
public:
    ~JZMqttResPacket();
    
    JZMqttMessage message;
};

#endif