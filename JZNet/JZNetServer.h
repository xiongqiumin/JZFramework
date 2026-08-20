#ifndef MG_SERVER_H_
#define MG_SERVER_H_

#include <map>
#include <deque>
#include "JZNetObject.h"
#include "JZTimer.h"

class JZNetClient;
class JZNetServer : public JZNetObject
{
public:
    JZNetServer(JZObject *parent = nullptr);
    ~JZNetServer();

    void setToken(std::string token);
    std::string token();

    bool listen(int port);
    bool listen(std::string ip, int port);
    void close();
    bool isListening() const;  

    void sendPacket(uint64_t c, const JZNetPack *pack);
    void sendPacketToAll(const JZNetPack *pack);
    void sendPacketToAllExclude(uint64_t c, const JZNetPack *pack);

    void closeClient(uint64_t c);

    // 取出一个待处理的新连接, 返回其连接 id (0 表示无新连接)
    uint64_t nextPendingConnection();

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

    void onHandTimer();
    void removeHandInfo(uint64_t conn);

    JZTimer *m_timer;
    std::string m_token;
    std::map<uint64_t, JZNetClient*> m_clients;
    std::deque<JZNetClient*> m_pendingConnections;
    // 记录每个连接进入握手(ConnectingState)的起始时间,用于超时判断
    std::map<uint64_t, int64_t> m_handTime;
};


#endif // ! MG_SERVER_H_
