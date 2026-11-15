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

    // 鉴权钩子: 在路由分发**之前**调用, 返回 false 表示"已自行回过包(如 401)", 本次请求结束。
    // 放在这里而不是塞进每个 handler: 路由有十几条, 漏一条就是漏洞。
    // 参数是原始 request(路径 / 查询串 / 头 / cookie 都可读)。
    void setAuthFilter(std::function<bool(JZHttpServerClient *, const JZHttpRequest &)> filter);

protected:
    friend JZHttpServerClient;

    virtual JZNetObject *onAccept(uint64_t conn, const JZNetAddr &addr) override;
    virtual void onNetEvent(JZNetResEvent *event) override;
    void onHttpMessage(JZHttpServerClient *client, const JZByteArray &buffer);

    JZHttpRoute m_route;
    std::function<bool(JZHttpServerClient *, const JZHttpRequest &)> m_authFilter;
};

#endif // ! MG_SERVER_H_
