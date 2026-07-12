#ifndef JZ_MCP_SERVER_H_
#define JZ_MCP_SERVER_H_

#include <string>
#include <functional>
#include "JZObject.h"

class JZMcpCore;
class JZHttpServer;
class JZHttpHandler;
class JZMcpHttpHandler;
class JZWebSocketServer;
class JZWebSocket;

/**
 * JZMcpServer —— MCP 后端传输层。
 *
 * 持有 JZMcpCore(协议核心), 并对外提供多种接入方式:
 *   - HTTP:    listenHttp(port, path)   —— Streamable HTTP, POST 端点
 *   - WS:      listenWebSocket(port)    —— WebSocket 文本帧
 *   - 纯文本:  processText(in, cb)       —— 异步处理 JSON 文本, 结果通过回调返回
 *
 * 协议语义(工具注册、JSON-RPC 分发)全部在 JZMcpCore 中完成,
 * JZMcpServer 只负责把传输层的输入转发给 core->process()、并把输出通过回调回送。
 */
class JZMcpServer : public JZObject
{
public:
    explicit JZMcpServer(JZObject* parent = nullptr);
    ~JZMcpServer();

    // 取协议核心(工具注册、serverInfo 设置等都通过它)
    JZMcpCore* core() const { return m_core; }

    // ---- 后端传输 ----
    // HTTP: Streamable HTTP, 在 port 监听, 端点路径 path(默认 "/mcp")
    bool listenHttp(int port, const std::string& path = "/mcp");
    // WebSocket: 在 port 监听, 文本帧即 JSON-RPC 请求
    bool listenWebSocket(int port);
    // 纯文本: 异步处理一段 JSON 文本, 结果通过回调返回。
    //   cb 签名: void(bool hasResponse, const std::string& outJson)
    //     - hasResponse: 是否有响应体(整段为通知时为 false, 此时 outJson 为空)
    //     - outJson:     响应文本(JSON-RPC 响应或解析错误)
    // 返回 true 表示已成功提交处理(结果稍后由 cb 异步回送)。
    using ProcessCallback = std::function<void(bool hasResponse, const std::string& outJson)>;
    bool processText(const std::string& in, ProcessCallback cb);

    void close();
    bool isListening() const;

private:
    friend class JZMcpHttpHandler;

    // WebSocket 回调
    void onWsAccept();
    void onWsMessage(const std::string& text);

    JZMcpCore*        m_core;
    JZHttpServer*     m_http;
    JZMcpHttpHandler* m_httpHandler;
    JZWebSocketServer* m_ws;
    std::string       m_path;
};

#endif // JZ_MCP_SERVER_H_
