#ifndef JZFRPCLIENT_H_
#define JZFRPCLIENT_H_

#include <map>
#include "JZFrpPacket.h"
#include "JZNet/JZNetClient.h"
#include "JZTcpSocket.h"
#include "JZTimer.h"

class JZFrpClientConfig
{
public:
    struct Proxy
    {
        std::string name;
        std::string localIp;
        int localPort;
        int remotePort;
    };

    JZFrpClientConfig();
    void addProxy(std::string name, std::string localIp, int localPort, int remotePort);

    std::string serverIp;
    int serverPort;
    std::string token;
    std::vector<Proxy> proxyList;
};


// FRP 客户端类, client 连接服务器后, 收到服务器数据,转发给本地
class JZFrpClient : public JZObject 
{
public:
    JZFrpClient();
    ~JZFrpClient();
        
    void setConfig(JZFrpClientConfig config);
    JZFrpClientConfig config();
    
    void connectToServer();
    void disconnectFromServer();
    bool waitForConnected(int ms);

jz_signals: 
    JZSigVoid sigConnected;

protected:
    struct LocalClientInfo
    {
        int serverPort;
        int connectId;
    };

    void onConnect();
    void onDisconnect();
    void onRecvPacket(JZNetPackPtr packet);
    void onCheckTimer();

    void onLocalConnect();
    void onLocalDisconnect();    
    void onLocalRead();
    void onLocalError(const std::string &error);
    void replyInvalidClient(int serverPort, int connectId);
   
    void clearLocalConnects();
    JZTcpSocket *localClient(int port, int id);
    void removeLocalClient(JZTcpSocket *c);
    void removeInvalidClient(JZTcpSocket *c);

    JZFrpClientConfig m_config;

    JZTimer *m_timer;
    JZNetClient *m_client;
    bool m_isValid;
    std::map<JZTcpSocket *, LocalClientInfo> m_localSocketMap;
};

#endif // JZFRPCLIENT_H_