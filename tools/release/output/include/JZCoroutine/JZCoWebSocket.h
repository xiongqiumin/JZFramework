#ifndef JZCO_WEB_SOCKET_H_
#define JZCO_WEB_SOCKET_H_

#include <queue>
#include "JZNetworkAccessManager.h"
#include "JZWebSocket.h"
#include "JZCoCoroutine.h"

//JZCoWebSocket
class JZCoWebSocketServer;
class JZCoWebSocket : public JZObject
{
public:
    enum  MessageType{ 
        None,
        Text, 
        Binary 
    };

	JZCoWebSocket(JZObject *object = nullptr);
    ~JZCoWebSocket();

    bool connectToHost(const JZNetworkRequest& req, int timeout = 5000);
    void disconnectFromHost();
    bool isConnect() const;

    void sendTextMessage(const std::string& message);
    void sendBinaryMessage(const JZByteArray& message);

    bool waitTextMessage(std::string& message, int timeout = -1);
    bool waitBinaryMessage(JZByteArray& message, int timeout = -1);
    MessageType waitAnyMessage(std::string& textMsg, JZByteArray& binMsg, int timeout = -1);

protected:
    friend JZCoWebSocketServer;

    JZCoWebSocket(JZObject* object, JZWebSocket* client);
    void connectSignals();

    enum {
        WaitNone,
        WaitConnect,
        WaitTextMessage,
        WaitBinaryMessage,
        WaitAnyMessage,
    };

    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const std::string& message);
    void onBinaryMessageReceived(const JZByteArray& message);
    void onError();

    void yield(int waitType, int timeout);
    bool getTextMessage(std::string& message);
    bool getBinaryMessage(JZByteArray& message);
    MessageType getAnyMessage(std::string& text, JZByteArray& bin);

    int m_waitType;
    std::queue<std::string> m_textQueue;
    std::queue<JZByteArray> m_binQueue;

    JZWebSocket* m_client;
    JZCoCondition m_coCtrl;
};

#endif // !JZCO_NET_WORK_H
