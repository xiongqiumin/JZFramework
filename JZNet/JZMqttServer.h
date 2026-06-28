#ifndef JZ_NET_MQTT_SERVER_H_
#define JZ_NET_MQTT_SERVER_H_

#include <map>
#include "JZMqttClient.h"
#include "JZOptional.h"

class JZMqttServer : public JZNetObject
{
public:
    JZMqttServer(JZObject *parent = nullptr);
    ~JZMqttServer();

    bool listen(int port);
    void close();
    bool isListening() const;

jz_signals: 
    JZSignal<JZMqttClient*> sigAccept;

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

    virtual JZNetObject *onAccept(uint64_t conn) override;
    virtual void onNetEvent(JZNetResEvent *event) override;
    void onClientDisconnect();
    void onClinetNetEvent(JZMqttClient *client, JZNetResEvent *event);
    JZOptional<SubscribePacket> parseSubscribeMessage(char *data, int size);

    uint64_t m_listen;
    std::map<std::string, std::vector<JZMqttClient *>> m_subcribeMap;
};

#endif  // ! MG_SERVER_H_