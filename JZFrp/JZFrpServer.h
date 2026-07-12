#ifndef JZFRPSERVER_H_
#define JZFRPSERVER_H_

#include "JZObject.h"
#include "JZFrpPacket.h"
#include "JZNet/JZNetServer.h"
#include "JZTcpServer.h"
#include <map>

class JZFrpServer;

class JZFrpServerConfig
{
public:
    std::string token;
};

// FRP 服务器类
class JZFrpServer : public JZObject {
public:
    JZFrpServer(JZObject *parent = nullptr);
    ~JZFrpServer();

    void setConfig(JZFrpServerConfig config);
    bool listen(int port);
    void close();

protected:
    struct RemoteInfo
    {
        RemoteInfo();
        ~RemoteInfo();

        void close();

        int port;
        JZTcpServer *listen;
        int clientId;
        std::map<int, JZTcpSocket *> remoteConnects;
    };

    // 外部连接信息结构体
    struct FrpConnection
    {
        FrpConnection();
        ~FrpConnection();

        void close(JZFrpServer *server);
        JZTcpSocket *remoteClient(int port, int id);

        bool valid;
        uint64_t frpc;  // FRP 客户端连接
        std::map<int, std::shared_ptr<RemoteInfo>> frps;  // 一个client 可能有多个端口，一个端口有多个客户
        std::string nonce;  // 随机数，用于认证
    };

    struct RemoteClientInfo
    {
        RemoteClientInfo();
        
        int serverPort;
        int connectId;
    };

    void onNewAccept();
    void onDisconnect(uint64_t);
    void onRecvPacket(uint64_t c, JZNetPackPtr packet);

    //rpc    
    void onFrpcAccept();
    void onFrpcRead();    
    void onFrpcClose();

    FrpConnection *getFrpClient(uint64_t c);
    FrpConnection *getFrpClientByPort(int port); //通过服务端口获取 FrpConnection, 可能多个端口对应一个FrpConnection
    void closeRemoteClients();
        
    JZFrpServerConfig m_config;
    JZNetServer *m_server;

    std::map<uint64_t, std::shared_ptr<FrpConnection>> m_frpClient;
    std::map<JZTcpSocket*, RemoteClientInfo> m_remoteInfo;
};

#endif // JZFRPSERVER_H_