#ifndef JZ_NET_MQTT_SERVER_H_
#define JZ_NET_MQTT_SERVER_H_
#include <map>
#include <deque>
#include "JZMqttClient.h"
#include "JZOptional.h"
#include "JZTimer.h"

class JZFRAMEWORK_API JZMqttServer : public JZNetObject
{
public:
    JZMqttServer(JZObject *parent = nullptr);
    ~JZMqttServer();

    bool listen(int port);
    void close();
    bool isListening() const;

    // MQTTS 服务端 TLS 配置见基类 JZNetObject::setSsl(caPem, certPem, keyPem)。

    JZMqttClient* nextPendingConnection();

jz_signals: 
    JZSigVoid sigAccept;

protected:
    friend JZMqttClient;

    struct SubscribeTopic
    {
        std::string topic;
        uint8_t qos;
    };

    struct SubscribePacket
    {
        uint16_t packetId;
        std::vector<SubscribeTopic> topics;
    };

    struct SubEntry
    {
        JZMqttClient *client;
        uint8_t qos;
    };

    struct ClientConnState
    {
        uint16_t keepAlive = 0;
        uint64_t lastActiveMs = 0;
        bool hasWill = false;
        JZMqttPublishOpt will;
    };

    virtual JZNetObject *onAccept(uint64_t conn, const JZNetAddr &addr) override;
    virtual void onNetEvent(JZNetResEvent *event) override;
    void onClientDisconnect();
    void onClientNetEvent(JZMqttClient *client, JZNetResEvent *event);
    JZOptional<SubscribePacket> parseSubscribeMessage(char *data, int size);
    JZOptional<ClientConnState> parseConnect(const JZByteArray &dgram);
    void onKeepaliveTimer();

    // 主题过滤器匹配(+ 单层级, # 多层级), 支持通配符
    static bool topicMatch(const std::string &filter, const std::string &topic);
    // 向新订阅者投递已保留(retained)消息
    void deliverRetained(JZMqttClient *client, const std::string &filter, uint8_t subQos);

    std::map<std::string, std::vector<SubEntry>> m_subcribeMap;
    std::map<std::string, JZMqttPublishOpt> m_retainedMap;
    std::map<JZMqttClient*, ClientConnState> m_clientState;
    std::deque<JZMqttClient*> m_pendingConnections;
    JZTimer *m_keepaliveTimer = nullptr;
};

#endif  // ! MG_SERVER_H_