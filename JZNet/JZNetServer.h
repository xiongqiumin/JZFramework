#ifndef MG_SERVER_H_
#define MG_SERVER_H_

#include <map>
#include <deque>
#include "JZNetObject.h"

class JZNetClient;
class JZNetServer : public JZNetObject
{
public:
    JZNetServer(JZObject *parent = nullptr);
    ~JZNetServer();

    void setToken(std::string token);
    std::string token();

    bool listen(int port);
    void close();
    bool isListening() const;  

    void sendPacket(uint64_t c, const JZNetPack *pack);
    void sendPacketToAll(const JZNetPack *pack);
    void sendPacketToAllExclude(uint64_t c, const JZNetPack *pack);

    void closeClient(uint64_t c);

    JZNetClient* nextPendingConnection();

jz_signals:    
    JZSigVoid sigAccept;
    JZSignal<uint64_t> sigDisconnect;
    JZSignal<uint64_t, JZNetPackPtr> sigPacketReceive;

protected:
    friend JZNetClient;

    void onClientDisconnect();
    void onClientPackReceive(JZNetPackPtr ptr);

    void onNetEvent(JZNetResEvent*event);
    virtual JZNetObject *onAccept(uint64_t conn) override;

    uint64_t m_listen;
    std::string m_token;
    std::map<uint64_t, JZNetClient*> m_clients;
    std::deque<JZNetClient*> m_pendingConnections;
};


#endif // ! MG_SERVER_H_
