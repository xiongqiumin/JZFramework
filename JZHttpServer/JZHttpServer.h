#ifndef JZ_HTTP_SERVER_H_
#define JZ_HTTP_SERVER_H_

#include <map>
#include <functional>
#include "JZNetObject.h"
#include "JZHttpRequest.h"
#include "JZHttpRespone.h"
#include "JZHttpRoute.h"

class JZHttpServer;
class JZHttpServerClient : public JZNetObject
{
public:
    JZHttpServerClient(JZObject *parent = nullptr);
    ~JZHttpServerClient();

    void response(const JZByteArray &buffer);
    void write(const JZByteArray &data, bool is_finish);  // 流式写入，不关闭连接

protected:
    friend JZHttpServer;

    virtual void onNetEvent(JZNetResEvent *event) override;
    void sendFinished();

    uint64_t m_conn;
};

class JZHttpServer : public JZNetObject
{
public:
    JZHttpServer(JZObject *parent = nullptr);
    ~JZHttpServer();

    bool listen(int port);
    void close();
    bool isListening() const;

    //path 语法 和 flask 类似, 具体见 JZHttpRoute 文档
    void addRoute(const std::string& path, JZHttpHandler* handler);

protected:
    friend JZHttpServerClient;

    virtual JZNetObject *onAccept(uint64_t conn) override;
    virtual void onNetEvent(JZNetResEvent *event) override;
    void onHttpMessage(JZHttpServerClient *client, const JZByteArray &buffer);

    uint64_t m_listen;
    JZHttpRoute m_route;
};

#endif // ! MG_SERVER_H_
