#ifndef JZ_NET_MQTT_CLIENT_H_
#define JZ_NET_MQTT_CLIENT_H_

#include "JZNetObject.h"
#include "JZMqttPacket.h"
#include "JZTimer.h"
#include <map>

class JZMqttServer;
class JZMgOptPacket;
class JZMqttClient : public JZNetObject
{
    JZ_OBJECT_DECLARE(JZNetMqttClient)

public:
    JZMqttClient(JZObject *parent = nullptr);
    ~JZMqttClient();

    SocketState state() const;

    void connectToHost(std::string url, const JZMqttConnectOpt &opt = JZMqttConnectOpt());
    bool waitForConnected(int ms = 30 * 1000);
    void disconnectFromHost();

    // MQTTS 客户端 TLS 配置见基类 JZNetObject::setSsl(caPem, clientCertPem, clientKeyPem, skipVerify)。
    
    int publish(const JZMqttPublishOpt &opt);
    void subscribe(const JZMqttSubOpt &opt);
    void unsubscribe(const JZMqttSubOpt &opt);

jz_signals:
    JZSigVoid sigConnect;
    JZSigVoid sigDisconnect;
    JZSignal<const JZMqttMessage &> sigMessage;

protected:
    friend JZMqttServer;

    struct MqttOutboundQos
    {
        uint16_t packetId;
        JZMqttPublishOpt opt;
        int64_t sendTimeMs;
    };

    void onNetEvent(JZNetResEvent*event);
    void closeInternal(bool is_error);
    void write(const std::shared_ptr<JZMgOptPacket> packet);
    void onQosTimer();
    void onPubAck(uint16_t id);
    void clearOutboundQos();

    JZMqttServer *m_server;
    SocketState m_state;

    // 出站 QoS1/2 可靠投递跟踪(内存级, 不依赖文件系统)
    std::map<uint16_t, MqttOutboundQos> m_outboundQos;
    uint16_t m_nextPacketId = 0;
    JZTimer *m_qosTimer = nullptr;
};

#endif