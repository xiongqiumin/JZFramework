#ifndef JZ_HTTP_SERVER_H_
#define JZ_HTTP_SERVER_H_
#include <map>
#include <functional>
#include "JZNetObject.h"
#include "JZHttpRequest.h"
#include "JZHttpRespone.h"
#include "JZHttpRoute.h"
#include "JZHttpHandler.h"

class JZHttpServer;
class JZFRAMEWORK_API JZHttpServerClient : public JZNetObject
{
public:
    JZHttpServerClient(JZObject *parent = nullptr);
    ~JZHttpServerClient();

    void setHandle(JZHttpHandler *handle);
    void response(const JZByteArray &buffer);
    // 发送完整响应并按请求的 Accept-Encoding 自动压缩正文(Content-Encoding: gzip/deflate)
    void response(const JZHttpRespone &resp, const JZHttpRequest &req);

    // 流式写入，is_finish=true 时结束响应（复用/关闭连接）
    void write(const JZByteArray &data, bool is_finish);  
    void write(const char *data, int size, bool is_finish);

protected:
    friend JZHttpServer;

    virtual void onNetEvent(JZNetResEvent *event) override;
    void sendFinished();

    JZHttpHandler *m_handle = nullptr;
    bool m_keepAlive = true;  // HTTP/1.1 默认 keep-alive, HTTP/1.0 由 Connection 头决定
    // 响应是否自定界: 含 Content-Length 或 Transfer-Encoding: chunked 时可复用 keep-alive 连接;
    // 否则(如 SSE 流式无 Content-Length)只能靠关闭连接来界定响应结束, sendFinished() 据此自动关连接。
    bool m_responseDelimited = true;
    bool m_responseStarted = false;   // 是否已从首段写入解析过响应头
    std::string m_headAccum;          // 累积响应头(头可能被拆分到多次写入)
};

class JZFRAMEWORK_API JZHttpServer : public JZNetObject
{
public:
    JZHttpServer(JZObject *parent = nullptr);
    ~JZHttpServer();

    bool listen(int port);
    bool listen(std::string ip, int port);
    void close();
    bool isListening() const;

    // HTTPS 服务端 TLS 配置见基类 JZNetObject::setSsl(caPem, certPem, keyPem)。
    // 例: server.setSsl("", serverCert, serverKey);  // 单向认证
    //     server.setSsl(caCert, serverCert, serverKey);  // 双向认证

    //path 语法 和 flask 类似, 具体见 JZHttpRoute 文档
    void addRoute(const std::string& path, JZHttpHandler* handler);

protected:
    friend JZHttpServerClient;

    virtual JZNetObject *onAccept(uint64_t conn, const JZNetAddr &addr) override;
    virtual void onNetEvent(JZNetResEvent *event) override;
    void onHttpMessage(JZHttpServerClient *client, const JZByteArray &buffer);

    JZHttpRoute m_route;
};

#endif // ! MG_SERVER_H_
