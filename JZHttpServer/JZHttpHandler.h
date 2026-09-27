#ifndef JZ_HTTP_HANDLER_H_
#define JZ_HTTP_HANDLER_H_
#include "JZObject.h"
#include "JZNetObject.h"
#include "JZHttpRequest.h"
#include "JZHttpRespone.h"

class JZHttpServerClient;

class JZFRAMEWORK_API JZHttpHandler : public JZObject
{
public:
    virtual ~JZHttpHandler() = default;
    virtual void handle(JZHttpServerClient* client, const JZHttpRequest& req) = 0;

    // 流式响应：底层 socket 可继续写入时由 JZHttpServerClient 回调。
    // 处理函数应通过 client->write(data, is_finish) 继续写入下一块数据；最后一块 is_finish=true 结束响应。
    // 连接断开清理：handler 自行 connect(client->sigDestroyed) 即可，无需框架额外回调。
    virtual void onNetEvent(JZHttpServerClient* client, JZNetResEvent *event) {}
};

#endif // JZ_HTTP_HANDLER_H_
