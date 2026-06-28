#ifndef JZ_NET_MQTT_CLIENT_H_
#define JZ_NET_MQTT_CLIENT_H_

#include "JZNetObject.h"
#include "JZMqttPacket.h"

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
    
    int publish(const JZMqttPublishOpt &opt);
    void subscribe(const JZMqttSubOpt &opt);
    void unsubscribe(const JZMqttSubOpt &opt);

jz_signals:
    JZSigVoid sigConnect;
    JZSigVoid sigDisconnect;
    JZSignal<const JZMqttMessage &> sigMessage;

protected:
    friend JZMqttServer;

    void onNetEvent(JZNetResEvent*event);
    void closeInternal(bool is_error);
    void write(const std::shared_ptr<JZMgOptPacket> packet);

    uint64_t m_client;
    JZMqttServer *m_server;
    SocketState m_state;
};

#endif