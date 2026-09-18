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
class JZHttpStream;
class JZTimer;

/**
 * JZMcpServer —— MCP 后端传输层。
 *
 * 持有 JZMcpCore(协议核心), 并对外提供多种接入方式:
 *   - HTTP:    listenHttp(port, path)   —— Streamable HTTP(POST 请求端点 + GET SSE
 *                                           推送流, 带心跳保活), 兼容 2025-11-25 /
 *                                           2025-06-18 与 2026-07-28
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
    // 注册 / 注销一条已打开的 SSE 流: handler 里 new JZHttpStream(client) + setSSE(true) +
    // open() 之后调 addSseClient(); handler 已 connect sigDestroyed 负责断开时注销。
    // 注册后服务器会自动给这些流发心跳(见 setSseHeartbeatInterval) —— 空闲的 SSE 流上
    // 没有任何数据, 客户端读体会超时(undici bodyTimeout), 别把心跳去掉。
    void addSseClient(JZHttpStream* stream);
    void removeSseClient(JZHttpStream* stream);
    // 当前是否有已连接的 GET SSE 流
    bool hasSseClients() const;
    // 向所有已连接的 GET SSE 流推送一条 JSON-RPC 通知(服务端主动推送):
    //   如 notifications/message(日志) / notifications/tools/list_changed 等。
    //   注意: 工具执行过程中的流式通知(sigNotify)仍按规范走 POST 响应内联 SSE,
    //   不应经此广播; 本接口用于与某次请求无关的服务器主动事件。
    void broadcastNotification(const std::string& jsonRpcNotification);
    // 便捷封装: 向所有 SSE 流推送一条 logging 通知(notifications/message)
    void logMessage(const std::string& level, const std::string& message);

    // SSE 心跳间隔(毫秒, 默认 15000)。SSE 流建立后若长时间没有数据往来, 客户端
    // (undici/fetch 等)的读体超时会到点报 "TypeError: terminated: Body Timeout Error",
    // 故空闲时按本间隔发一个 ": ping" 注释帧保活(注释帧不会被客户端当成事件)。
    // <= 0 关闭心跳(那就得自己保证有数据推送)。
    // ⚠ 需在 server 所在线程调用(定时器有线程亲和性)。
    void setSseHeartbeatInterval(int ms);
    int sseHeartbeatInterval() const { return m_sseHeartbeatMs; }

    void close();
    bool isListening() const;

private:
    friend class JZMcpHttpHandler;

    // GET /mcp 建立的 SSE 推送流(stream 以 JZHttpServerClient 为父对象 ⇒ 客户端断开即析构;
    // guard 据此判断"还活着吗", 广播/心跳时跳过已断开的流, 不会悬空写入)。
    // 跨线程访问由 m_sseMutex 保护。
    struct SseClient
    {
        JZObjectPointer guard;   // 流销毁后 object() == nullptr
        JZHttpStream*   stream;
    };

    // 快照后逐个回调(锁内不回调网络, 免得持锁写)
    void forEachSseClient(const std::function<void(JZHttpStream*)>& fn);
    // 心跳: 给所有活着的流发 ": ping" 注释帧; 没有订阅者了就停掉定时器
    void heartbeatTick();
    // 启动心跳定时器(必须在 server 线程: JZObject::startTimer 有线程断言)
    void ensureSseHeartbeatStarted();

    JZMcpCore*        m_core;
    JZHttpServer*     m_http;
    JZMcpHttpHandler* m_httpHandler;
    std::string       m_path;

    mutable std::mutex      m_sseMutex;
    std::vector<SseClient>  m_sseClients;
    JZTimer*                m_sseHeartbeat = nullptr;
    int                     m_sseHeartbeatMs = 15000;
};

#endif // JZ_MCP_SERVER_H_
