#ifndef JZ_NET_MQTT_SERVER_H_
#define JZ_NET_MQTT_SERVER_H_

#include "JZNetObject.h"
#include "JZNetMqttPacket.h"

class JZNetMqttServer;
class JZNetMqttClient : public JZNetObject
{
    JZ_OBJECT_DECLARE(JZMqttClient)

public:
    JZMqttClient(JZObject *parent = nullptr);
    ~JZMqttClient();

    SocketState state() const;

    void connectToHost(std::string ip, int port);
    bool waitForConnected(int ms = 30 * 1000);
    void disconnectFromHost();

    void write(const JZNetMqttPacket &pack);
    
jz_signals:
    JZSigVoid sigConnect;
    JZSigVoid sigDisconnect;
    JZSignal<JZNetMqttPacket> sigRecvPacket;

protected:
    friend JZNetMqttServer;

    void onNetEvent(JZNetResEvent*event);
    void closeInternal(bool is_error);

    JZByteArray m_buffer;

    uint64_t m_client;
    SocketState m_state;
};

#endif