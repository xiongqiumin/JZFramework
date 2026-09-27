#ifndef JZ_TCP_SERVER_H_
#define JZ_TCP_SERVER_H_
#include <map>
#include <deque>
#include "JZNetObject.h"
#include "JZTcpSocket.h"

class JZFRAMEWORK_API JZTcpServer : public JZNetObject
{
public:
    JZTcpServer(JZObject *parent = nullptr);
    ~JZTcpServer();

    bool listen(int port);
    bool listen(std::string ip, int port);
    int port() const;

    void close();
    bool isListening() const;

    JZTcpSocket* nextPendingConnection();

jz_signals:    
    JZSigVoid sigAccept;    

protected:
    virtual JZNetObject *onAccept(uint64_t conn) override;
    virtual void onNetEvent(JZNetResEvent* event) override;

    int m_port;
    std::deque<JZTcpSocket*> m_pendingConnections;
};

#endif // ! MG_SERVER_H_
