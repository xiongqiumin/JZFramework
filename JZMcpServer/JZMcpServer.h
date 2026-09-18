#ifndef JZ_MCP_SERVER_H_
#define JZ_MCP_SERVER_H_

#include <string>
#include <functional>
#include <vector>
#include <mutex>
#include "JZObject.h"

class JZMcpCore;
class JZHttpServer;
class JZHttpHandler;
class JZMcpHttpHandler;
class JZHttpServerClient;

/**
 * JZMcpServer —— MCP 后端传输层。
 *
 * 持有 JZMcpCore(协议核心), 并对外提供多种接入方式:
 *   - HTTP:    listenHttp(port, path)   —— Streamable HTTP(POST 端点 + GET SSE 推送流,
 *                                           兼容 2025-06-18 与 2026-07-28)
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
    bool listenHttp(std::string ip, int port, const std::string& path = "/mcp");
    // 纯文本: 异步处理一段 JSON 文本, 结果通过回调返回。
    //   cb 签名: void(bool hasResponse, const std::string& outJson, bool finished)
    //     - hasResponse: 是否有响应体(整段为通知时为 false, 此时 outJson 为空)
    //     - outJson:     响应文本(JSON-RPC 响应 / 通知 / 解析错误)
    //     - finished:    true=该请求最后一个分块(结束响应); false=流式中间分块(保持连接)
    // 返回 true 表示已成功提交处理(结果稍后由 cb 异步回送)。
    using ProcessCallback = std::function<void(bool hasResponse, const std::string& outJson, bool finished)>;
    bool processText(const std::string& in, ProcessCallback cb);

    // ---- GET SSE 推送流(服务端 → 客户端) ----
    // 注册 / 注销一条已建立的 GET /mcp SSE 连接(连接断开时自动注销, 无需手动调用)。
    void addSseClient(JZHttpServerClient* client);
    void removeSseClient(JZHttpServerClient* client);
    // 当前是否有已连接的 GET SSE 流
    bool hasSseClients() const;
    // 向所有已连接的 GET SSE 流推送一条 JSON-RPC 通知(服务端主动推送):
    //   如 notifications/message(日志) / notifications/tools/list_changed 等。
    //   注意: 工具执行过程中的流式通知(sigNotify)仍按规范走 POST 响应内联 SSE,
    //   不应经此广播; 本接口用于与某次请求无关的服务器主动事件。
    void broadcastNotification(const std::string& jsonRpcNotification);
    // 便捷封装: 向所有 SSE 流推送一条 logging 通知(notifications/message)
    void logMessage(const std::string& level, const std::string& message);

    void close();
    bool isListening() const;

private:
    friend class JZMcpHttpHandler;

    JZMcpCore*        m_core;
    JZHttpServer*     m_http;
    JZMcpHttpHandler* m_httpHandler;
    std::string       m_path;

    // GET /mcp 建立的 SSE 推送流(client 以 guardPointer 持有, 销毁后 object() 返回 nullptr,
    // 广播时据此跳过已断开的连接, 避免悬空指针)。跨线程访问由 m_sseMutex 保护。
    struct SseClient
    {
        JZObjectPointer     guard;   // 对象销毁后 object() == nullptr
        JZHttpServerClient* client;
    };
    mutable std::mutex      m_sseMutex;
    std::vector<SseClient>  m_sseClients;
};

#endif // JZ_MCP_SERVER_H_
