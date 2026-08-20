#ifndef MG_CLIENT_H_
#define MG_CLIENT_H_

#include <string>
#include "JZNetObject.h"
#include "JZNetPacket.h"
#include "JZNetPacketProcessor.h"

class JZNetServer;
class JZNetClient : public JZNetObject
{
public:
    JZNetClient(JZObject *parent = nullptr);
    ~JZNetClient();

    SocketState state() const;

    void setToken(std::string token);
    std::string token();

    void connectToHost(std::string ip, int port);
    void disconnectFromHost();
    bool isConnect();
    bool waitForConnected(int ms);

    void sendPacket(const JZNetPack *packet);
    JZNetPackPtr waitPacket(int ms = 5 * 1000);
    JZNetPackPtr waitPacketByType(int packet_type,int ms = 5 * 1000);        
    void clearPacket();    
    
    std::string lastError();    
    void clearLastError();
   
jz_signals:
    JZSigVoid sigConnect;
    JZSigVoid sigDisconnect;
    JZSignal<JZNetPackPtr> sigPacketReceive;

protected:
    friend JZNetServer;

    void onNetEvent(JZNetResEvent*event);
    void setLastError(std::string error);    
    void closeInternal(bool is_error);
    void sendHandPacket(const JZNetPack *packet);
    
    JZNetPacketProcessor m_packet;
    std::string m_error;        
    bool m_waitPacket;
    SocketState m_state;
    JZNetServer *m_server;
    uint64_t m_serverId;
    std::string m_token;
};







#endif