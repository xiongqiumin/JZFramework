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

protected:
    friend JZHttpServer;

    void response(const JZByteArray &buffer);
    virtual void onNetEvent(JZNetResEvent *event) override;

    uint64_t m_conn;
};

class JZHttpServer : public JZNetObject
{
public:
    using Handler = std::function<void(const JZHttpRequest&, JZHttpRespone&)>;

    JZHttpServer(JZObject *parent = nullptr);
    ~JZHttpServer();

    bool listen(int port);
    void close();
    bool isListening() const;

    void addRoute(const std::string& path, Handler handler);

protected:
    friend JZHttpServerClient;

    virtual JZNetObject *onAccept(uint64_t conn) override;
    virtual void onNetEvent(JZNetResEvent *event) override;
    void onHttpMessage(JZHttpServerClient *client, const JZByteArray &buffer);

    uint64_t m_listen;
    JZHttpRoute m_route;
};

#endif // ! MG_SERVER_H_
