#ifndef JZ_TS_LANG_SERVER_H_
#define JZ_TS_LANG_SERVER_H_

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>

#include "JZObject.h"
#include "JZProcess.h"
#include "json/json.h"

// 10 个 TS 工具对应的查询类型，与 JZTsTool 一一对应
enum class TsMethod
{
    Definition,         // ts_definition
    References,         // ts_references
    QuickInfo,          // ts_quickinfo
    Rename,             // ts_rename
    Completion,         // ts_completion
    DocumentHighlights, // ts_document_highlights
    Diagnostics,        // ts_diagnostics
    Source,             // ts_source
    Outline,            // ts_outline
    Members             // ts_members
};

// 直连 TSGO 语言服务的 C++ LSP 客户端。
//
// TSGO 的原生二进制就是 tsc.exe，它内置了 LSP 语言服务器，启动方式：
//     tsc.exe --lsp --stdio
// 本类 spawn 该进程，按 "JSON-RPC over Content-Length" 的 LSP 协议与之通信，
// 对外以 agent tool 的形式暴露 10 个 TS 查询（不经 MCP / HTTP 包装层）。
//
// 进程常驻、跨多次查询复用（引擎冷启动代价高，与 mcp-server.js 的单例设计一致）。
// 与之前 node + tsgo_lang_server.js 桥接方案不同：这里不再依赖 node，
// 直接驱动原生 TSGO LSP 服务器。
class JZTsLangServer : public JZObject
{
public:
    explicit JZTsLangServer(JZObject *parent = nullptr);
    ~JZTsLangServer();

    // 原生 tsgo(tsc) 二进制全路径；isAvailable 校验其存在
    void setTsgoPath(const std::string &tsc);
    bool isAvailable() const;

    // 工作区根目录，作为 LSP initialize 的 rootUri / workspaceFolders
    void setRoot(const std::string &root);

    // 单个 LSP 请求无回复的超时(ms)：到期判定语言服务卡死并触发重启；
    // 设为 <=0 关闭超时重启（默认 30000）。
    void setRequestTimeout(int ms);

    // 诊断/测试用：当前是否已握手就绪；以及强制杀掉底层进程以模拟崩溃
    // （杀掉后会走 onFinished -> 异常自愈 -> 自动重启）。
    bool isReady() const { return m_ready; }
    void killProcessForTest();

    // 统一查询入口：method 为查询类型；params = {file, line(1-based), offset(1-based), ...}
    // 回调 (ok, payload)：ok 时 payload 为结果 JSON 字符串，否则为错误文本。
    using TsCallback = std::function<void(bool ok, const std::string &payload)>;
    void request(TsMethod method, const Json::Value &params, TsCallback cb);

    // 终止底层进程并丢弃所有未决回调（停止 / 重建 agent 时调用，避免悬垂回调）
    void shutdown();

jz_signals:
    JZSignal<const std::string &> sigLog;

protected:
    // 进程与握手
    void ensureStarted();
    void onStarted();
    void onFinished(int exitCode, JZProcess::ExitStatus status);
    void onErrorOccurred(JZProcess::ProcessError err);
    void onStdOutReady();
    void onStdErrReady();

    // 异常自愈：进程退出 / 进程错误 / 请求超时后，失败在途请求并重启进程
    void handleProcessGone(const std::string &reason);
    void failAllPending(const std::string &reason);
    void scheduleRestart();  // 带退避的一次性重启定时器
    void restartNow();
    void checkTimeouts();    // 看门狗：检查在途请求是否超时
    void timerEvent(JZTimerEvent *event) override;

    // LSP 协议
    void sendFrame(const Json::Value &msg);
    void sendNotification(const std::string &method, const Json::Value &params);
    void callLsp(const std::string &method, const Json::Value &params,
                 std::function<void(const Json::Value &)> onResult,
                 std::function<void(const std::string &)> onError);
    void handleMessage(const Json::Value &msg);
    void handleNotification(const Json::Value &msg);

    // 文档同步
    std::string ensureOpen(const std::string &file);
    static std::string readFileUtf8(const std::string &file);

    // URI 与位置
    static std::string fileToUri(const std::string &file);
    static std::string uriToFile(const std::string &uri);
    static std::string langIdOf(const std::string &file);

    // 工具分发
    void dispatch(TsMethod method, const Json::Value &params, TsCallback cb);

    // 结果归一化辅助
    static Json::Value locToObj(const Json::Value &loc);
    static Json::Value normalizeLocations(const Json::Value &r);
    static Json::Value firstLocation(const Json::Value &def);
    Json::Value readFileRange(const std::string &file, const Json::Value &range);
    static Json::Value symbolAt(const Json::Value &syms, const Json::Value &pos);
    static std::string markupToString(const Json::Value &c);

    // 按名字在 documentSymbol 树中解析符号（供大模型以 name 代替精确行列）。
    // findSymbolByName 优先返回 class/interface，找不到再退回同名首个匹配。
    static Json::Value findSymbolByName(const Json::Value &syms, const std::string &name);
    static Json::Value findSymbolByNameImpl(const Json::Value &syms,
                                            const std::string &name, bool preferClass);
    // file + name -> 解析出符号的 (line, character)（LSP 坐标），失败走 onErr。
    // 用于 Members / References 在不传 line/offset 时按名字定位。
    void resolveByName(const std::string &file, const std::string &name,
                       std::function<void(int line, int character)> onPos,
                       std::function<void(const std::string &)> onErr);

    struct LspReq
    {
        std::function<void(const Json::Value &)> onResult;
        std::function<void(const std::string &)> onError;
        int64_t enqueueMs = 0;   // 发出时刻（用于超时判定）
    };

    struct OpenDoc
    {
        int version = 0;
        std::string text;
    };

    struct QueuedReq
    {
        TsMethod method;
        Json::Value params;
        TsCallback cb;
    };

    JZProcess *m_proc = nullptr;
    bool m_ready = false;
    bool m_alive = true;
    std::string m_tsc;
    std::string m_root;

    int m_nextId = 1;
    std::map<int, LspReq> m_pending;        // 已发出、等待响应的 JSON-RPC 请求
    std::deque<QueuedReq> m_queue;          // 握手未完成时排队的查询
    std::map<std::string, OpenDoc> m_docs;  // 已 open 的文档(uri -> 内容/版本)

    std::string m_buf;                      // stdout 字节缓冲（解析 Content-Length 帧）

    // 异常自愈相关
    int m_watchdogTimer = 0;                // 周期检查在途请求超时
    int m_restartTimer = 0;                 // 退避后重启进程
    bool m_restartPending = false;
    int64_t m_restartBackoff = 0;           // 连续失败退避(ms)，成功握手后清零
    int64_t m_requestTimeoutMs = 30000;     // 单请求超时(ms)，<=0 表示关闭
};

#endif // !JZ_TS_LANG_SERVER_H_
