#include "JZAgentCli.h"

#include "JZByteArray.h"
#include "JZDebug.h"
#include "JZDir.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZNetworkAccessManager.h"   // 联网工具共享的访问管理器(+ JZProxyConfig)
#include "JZPlatform.h"
#include "JZProcess.h"       // 验证命令的异步执行(超时/进程树收尾)
#include "JZString.h"        // str_toLower
#include "JZVirtualTerminal.h"
#include "json/json.h"
#include "JZLog.h"              // JZLogManager / LogModule_Agent / LOG_INFO
#include "JZUtf8Iterator.h"     // 验证输出按字符(码点)截断, 不切坏多字节字符

#include <algorithm>            // std::min(日志摘要里取首行片段)
#include <cstdio>               // snprintf(SSE chunked 长度前缀)
#include <fstream>              // 网页页面从磁盘读
#include <memory>               // shared_ptr(异步回包的状态)
#include <sstream>
#include <string>

// 回合诊断日志: 把工具调用 / 思考 / 回复 / 输入 记进独立的 Agent 日志模块(模块 id = LogModule_Agent)。
// 该模块不走 Console, 只有配置 logFile 才落盘到 <logFile目录>/Agent.log, 没配就不打印。
//
// 精简口径(骨架优先, 大文本只留摘要): 日志是给人**排查流程**看的, 不是模型上下文的备份 ——
//   * 工具行: "<工具> args=<简写> => ok/fail <结果摘要>"。长结果一律折叠(只留头 + 尾 + 总长),
//     read_file 成功更进一步只记大小(整份文件内容对排查没意义, 要看直接看文件)。
//   * think/reply/input: 只记"单行摘要"(标签 + 字节数 + 首行片段), 不写全文 —— 长思考/长回复
//     整段落盘会把日志刷成几万行, 真正要复读全文看的是对话历史 / 网页事件流, 不是日志。
namespace {
const int kToolResultBytes = 300;   // 工具结果摘要里保留的字节数(超出折叠成 头…尾)
const int kTextPreviewBytes = 200;  // think/reply/input 摘要里保留的首行字节数

// 单行化: 换行/制表符折叠成空格, 便于日志一条占一行
std::string logOneLine(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    return out;
}

// UTF-8 边界工具: 日志与验证输出都可能是中文/多字节, 按**字节** substr 会把一个字切成
// 半个(日志里表现为结尾一个乱码字符)。统一走框架的 JZUtf8Iterator(码点迭代 + byteIndex):
//   utf8CutAtByteLimit  = 按字节上限取前缀, 落点对齐到码点边界
//   utf8TailAtByteLimit = 按字节上限取后缀, 起点对齐到码点边界
//   utf8CountChars / utf8CutAtChar / utf8TailAtChar = 按码点个数取(验证输出用)
// 第 n 个码点的字节起始下标(不足 n 个码点则返回全长)
static int utf8CharByteOffset(const std::string &s, size_t chars)
{
    JZUtf8Iterator it(s);
    int n = 0;
    while (it.isValid() && (size_t)n < chars)
    {
        ++it;
        n++;
    }
    return it.byteIndex();
}

static size_t utf8CountChars(const std::string &s)
{
    return (size_t)JZUtf8Iterator::countCodepoints(s);
}

static std::string utf8CutAtChar(const std::string &s, size_t chars)
{
    return s.substr(0, (size_t)utf8CharByteOffset(s, chars));
}

// 按**字节上限**截断, 但落点对齐到码点边界(不留半个多字节字符)。
static std::string utf8CutAtByteLimit(const std::string &s, size_t maxBytes)
{
    if (s.size() <= maxBytes)
        return s;
    JZUtf8Iterator it(s);
    int last = 0;
    while (it.isValid() && (size_t)it.byteIndex() < maxBytes)
    {
        last = it.byteIndex();
        ++it;
    }
    return s.substr(0, (size_t)last);
}

// 从尾部按字节上限取后缀, 起点落在码点边界上(不会以半个字开头)。
static std::string utf8TailAtByteLimit(const std::string &s, size_t maxBytes)
{
    if (s.size() <= maxBytes)
        return s;
    const size_t floor = s.size() - maxBytes;
    JZUtf8Iterator it(s);
    size_t start = s.size();
    while (it.isValid())
    {
        if ((size_t)it.byteIndex() >= floor)
        {
            start = (size_t)it.byteIndex();
            break;
        }
        ++it;
    }
    return s.substr(start);
}

static std::string utf8TailAtChar(const std::string &s, size_t chars)
{
    size_t total = utf8CountChars(s);
    if (total <= chars)
        return s;
    return s.substr((size_t)utf8CharByteOffset(s, total - chars));
}

// 结果/文本摘要: 短就原样单行化; 长就给 "头…尾[共N字节]"。用于工具结果。
// 头尾都按 UTF-8 码点边界截, 中文不会被切成半个字。
std::string logSummary(const std::string &s, int keepBytes)
{
    if (keepBytes <= 0)
        return std::string();
    if ((int)s.size() <= keepBytes)
        return logOneLine(s);
    const size_t head = (size_t)keepBytes / 2;
    const size_t tail = (size_t)keepBytes - head;
    return logOneLine(utf8CutAtByteLimit(s, head)) + " … "
           + logOneLine(utf8TailAtByteLimit(s, tail))
           + "[共 " + std::to_string(s.size()) + " 字节]";
}

// Agent 模块日志。JZLog 多模块: 每个模块独立 type(Console/File/None), 互不干扰。
static void agentLog(const std::string &msg)
{
    JZLogManager::instance()->log(LogModule_Agent, LOG_INFO, msg);
}

// think/reply/input 摘要: 标签 + "N 字节" + 首行片段(不写全文)。
// 预览按 UTF-8 码点边界截, 结尾的 "…" 表示后面还有内容, 不是半个汉字。
static std::string logTextLine(const char *tag, const std::string &text)
{
    const std::string head = utf8CutAtByteLimit(text, (size_t)kTextPreviewBytes);
    std::string line = std::string(tag) + "[" + std::to_string(text.size()) + " 字节] " + logOneLine(head);
    if (head.size() < text.size())
        line += " …";
    return line;
}
}

#include "JZHttpServer.h"     // HTTP 服务(路由 / JZHttpServerClient / JZHttpRespone)
#include "JZHttpHandler.h"
#include "JZHttpStaticFiles.h"

#include "JZNet/JZNetPacket.h"  // JZNetPackInit(frpc 的 JZNetClient 依赖包注册)
#include "JZAgentNodePacket.h"  // link 模式 node 的执行面协议(remote_ssh)
#include "JZAgentHubPacket.h"   // 控制面: cli <-> server 的任务下发/上报
#include "JZNodeLink.h"         // cli -> node 的连接与 remote_ssh 工具
#include "JZHubLink.h"          // cli -> server 的控制面连接(managed 模式)

#include <cstdio>
#include <cstdlib>            // atoi(解析 "HTTP 402 ..." 里的状态码)
#ifndef _WIN32
#include <sys/wait.h>         // WIFEXITED / WEXITSTATUS(popen 取退出码)
#endif

#include "JZTimer.h"          // 重试前的延时(JZTimer::singleShot)

#include "JZTerminalTools.h"  // 本地终端工具(JZTerminalRunTool 等)
#include "JZMcpClient.h"      // MCP 客户端(连外部 MCP 服务器, 把远端工具挂进本 agent)

#include <iostream>
#include <fstream>            // system.extraFile(宿主常驻说明)读文件
#include <sstream>
#include <thread>
#include <chrono>             // 历史记录的起止时间戳(nowMs)

using namespace std;

const int JZAgentCli::kHistoryMax;

// 单次读文件上限(与 node 一致): 避免把几百兆读进内存。
static const int kReadFileLimit = 8 * 1024 * 1024;

// 验证输出回包上限(与旧 popen 实现一致): 几十 KB 足够定位, 再大只会撑爆 JSON 回包与上下文。
static const size_t kMaxVerifyBytes = 65536;

// /task 的图片附件上限(张数 / 单张字符数 / 合计字符数)。
//   张数与 code_agent 输入框的附图上限对齐; 字符上限按"截图 base64 通常几百 KB"给足余量,
//   但必须有硬上限 —— 整个 body 都要进内存, 还要再塞进 LLM 请求体。
//   ⚠ 合计上限必须明显小于 main.cpp 里抬高的 net maxRecvSize(32MB), 否则先被 mongoose
//     的接收缓冲上限掐断(客户端只看到 connection aborted, 没有任何错误回包)。
static const int kMaxTaskImages = 8;
static const int kMaxImageChars = 8 * 1024 * 1024;
static const int kMaxImagesTotalChars = 24 * 1024 * 1024;

// JZByteArray -> std::string(进程输出按字节接回来, 原样存)
static std::string bytesToText(const JZByteArray &data)
{
    if (data.size() <= 0 || !data.data())
        return std::string();
    return std::string(data.data(), data.size());
}

// UTF-8 边界工具集中在文件顶部的匿名命名空间(日志摘要与验证输出共用同一份实现)。

// 临时目录(放验证命令的包装脚本用): TEMP/TMPDIR 都没有就退回当前目录。
static std::string tempDirPath()
{
#ifdef _WIN32
    const char *t = std::getenv("TEMP");
    if (!t || !*t)
        t = std::getenv("TMP");
#else
    const char *t = std::getenv("TMPDIR");
#endif
    std::string dir = (t && *t) ? std::string(t) : std::string(".");
    for (size_t i = 0; i < dir.size(); i++)
    {
        if (dir[i] == '\\')
            dir[i] = '/';
    }
    while (dir.size() > 1 && dir.back() == '/')
        dir.pop_back();
    return dir;
}

// ==========================================================================
// JZCliToolHost
// ==========================================================================

JZCliToolHost::JZCliToolHost(const JZCliConfig *config)
    : m_config(config)
{
}


// ==========================================================================

// 路由处理器: 把 /task 请求转交给 JZAgentCliTask::onTaskSubmit
class TaskHandler : public JZHttpHandler
{
public:
    explicit TaskHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onTaskSubmit(client, req);
    }
private:
    JZAgentCli *m_cli;
};

// 路由处理器: 列出工作区内可当验证脚本的文件, 供网页下拉选择
// 网页: / 直接给对话页(agent.html) —— 与 server 的 /agent 是同一份页面
class DashboardHandler : public JZHttpHandler
{
public:
    explicit DashboardHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onDashboard(client, req);
    }
private:
    JZAgentCli *m_cli;
};

// A2A 网页接口(与 server 管理台同一套 /api/*)
class ApiTasksHandler : public JZHttpHandler
{
public:
    explicit ApiTasksHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onApiTasks(client, req);
    }
private:
    JZAgentCli *m_cli;
};

class ApiTaskDetailHandler : public JZHttpHandler
{
public:
    explicit ApiTaskDetailHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onApiTaskDetail(client, req);
    }
private:
    JZAgentCli *m_cli;
};

class ApiTaskCancelHandler : public JZHttpHandler
{
public:
    explicit ApiTaskCancelHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onApiTaskCancel(client, req);
    }
private:
    JZAgentCli *m_cli;
};

class ApiTaskDeleteHandler : public JZHttpHandler
{
public:
    explicit ApiTaskDeleteHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onApiTaskDelete(client, req);
    }
private:
    JZAgentCli *m_cli;
};

class ApiTaskSubscribeHandler : public JZHttpHandler
{
public:
    explicit ApiTaskSubscribeHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onApiTaskSubscribe(client, req);
    }
private:
    JZAgentCli *m_cli;
};

class ApiAgentHandler : public JZHttpHandler
{
public:
    explicit ApiAgentHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onApiAgent(client, req);
    }
private:
    JZAgentCli *m_cli;
};

class ApiStatusHandler : public JZHttpHandler
{
public:
    explicit ApiStatusHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onApiStatus(client, req);
    }
private:
    JZAgentCli *m_cli;
};

class ScriptsHandler : public JZHttpHandler
{
public:
    explicit ScriptsHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onListScripts(client, req);
    }
private:
    JZAgentCli *m_cli;
};

// 路由处理器: 实时事件流(GET /events, SSE)
class EventsHandler : public JZHttpHandler
{
public:
    explicit EventsHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onEvents(client, req);
    }
private:
    JZAgentCli *m_cli;
};

// 路由处理器: 运行信息(GET /info)
class InfoHandler : public JZHttpHandler
{
public:
    explicit InfoHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onInfo(client, req);
    }
private:
    JZAgentCli *m_cli;
};

// 路由处理器: 终端面板(GET /terminal, POST /terminal/cancel)
class TerminalListHandler : public JZHttpHandler
{
public:
    explicit TerminalListHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onTerminalList(client, req);
    }
private:
    JZAgentCli *m_cli;
};

class TerminalCancelHandler : public JZHttpHandler
{
public:
    explicit TerminalCancelHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onTerminalCancel(client, req);
    }
private:
    JZAgentCli *m_cli;
};

// 路由处理器: 重置会话(POST /reset)
class ResetHandler : public JZHttpHandler
{
public:
    explicit ResetHandler(JZAgentCli *cli) : m_cli(cli) {}
    void handle(JZHttpServerClient *client, const JZHttpRequest &req) override
    {
        m_cli->onReset(client, req);
    }
private:
    JZAgentCli *m_cli;
};

bool JZAgentCli::init(const JZCliConfig &config, std::string &error)
{
    m_config = config;
    if (m_config.rootDir.empty())
        m_config.rootDir = JZDir::currentPath();
    m_config.rootDir = codeNormalizePath(m_config.rootDir);
    if (m_config.rootDir.empty())
        m_config.rootDir = codeNormalizePath(JZDir::currentPath());
    for (TrustedDir &dir : m_config.trustedDirs)
        dir.path = codeNormalizePath(dir.path);

    // project = rootDir(上面已兜底), 不再有单独的名字; 网页显示时取末段目录名即可。
    // 全局 .jzagent(settings 那份): 里面的 skills 总是加载; 默认 %USERPROFILE%/.jzagent
    if (m_config.skillsDir.empty())
    {
        const char *home = getenv("USERPROFILE");
        if (home && *home)
            m_config.skillsDir = string(home) + "/.jzagent";
    }
    m_config.skillsDir = codeNormalizePath(m_config.skillsDir);

    // 任务池: 启动时一个 task 都不建, 第一次提交(HTTP / hub)时按需创建。
    m_tasks.clear();
    m_pending.clear();
    // 空闲回收不能只靠任务结束那一下: 之后一直没新任务来就永远收不掉。
    m_recycleTimer = new JZTimer(this);
    connect(&m_recycleTimer->sigTimeout, this, [this]() { recycleIdleTasks(); });
    m_recycleTimer->start(5000);
    error.clear();
    return true;
}

std::string JZAgentCli::resolveRootDir(const Json::Value &req, std::string &error) const
{
    std::string want = req.isObject() ? req.get("rootDir", "").asString() : std::string();
    if (want.empty())
        return m_config.rootDir;      // 没指定 = 用配置的根目录
    want = codeNormalizePath(want);
    JZFileInfo info(want);
    if (!info.exists() || !info.isDir())
    {
        error = "工作目录不存在: " + want;
        return std::string();
    }
    return want;
}

JZAgentCliTask *JZAgentCli::acquireTask(const std::string &wantRoot, std::string &error)
{
    const std::string root = wantRoot.empty() ? m_config.rootDir : wantRoot;
    // **不复用任务对象**: 一个任务 = 一个全新的 JZAgent + 终端 + 工具表 + 系统提示。
    // 重建就是 new 几个对象(毫秒级), 省这点开销不值得冒"残留状态串到下一轮"的风险;
    // 工作目录/模型/技能这些也天然不会串。所以这里只处理"腾位置 + 新建"。
    if ((int)m_tasks.size() >= m_config.task.queues)
    {
        for (auto it = m_tasks.begin(); it != m_tasks.end(); ++it)
        {
            JZAgentCliTask *old = *it;
            if (old->busy() || !old->taskId().empty())
                continue;
            m_tasks.erase(it);
            old->deleteLater();
            agentLog("丢弃已结束的任务对象, 给新任务腾位置(剩余 "
                     + std::to_string(m_tasks.size()) + " 个)");
            break;
        }
    }
    // 活动数到上限 -> 交给调用方排队
    if ((int)m_tasks.size() >= m_config.task.queues)
        return nullptr;
    JZAgentCliTask *t = new JZAgentCliTask(this, this);
    JZCliConfig cfg = m_config;
    cfg.rootDir = root;
    if (!t->init(cfg, error))
    {
        t->deleteLater();
        return nullptr;
    }
    for (JZMcpClient *c : m_mcpClients)
        t->agent()->addMcp(c);
    m_tasks.push_back(t);
    agentLog("创建任务对象(共 " + std::to_string(m_tasks.size())
             + " 个, 上限 " + std::to_string(m_config.task.queues) + ")");
    return t;
}

void JZAgentCli::releaseTask(JZAgentCliTask *task)
{
    if (!task)
        return;
    auto it = std::find(m_tasks.begin(), m_tasks.end(), task);
    if (it == m_tasks.end())
        return;                     // 已在腾位置时被丢了
    m_tasks.erase(it);
    task->deleteLater();
    agentLog("任务结束, 销毁任务对象(剩余 " + std::to_string(m_tasks.size()) + " 个)");
}

void JZAgentCli::onTaskFinished(JZAgentCliTask *task)
{
    if (task)
        task->markIdle(JZAgentCliTask::nowMs());
    syncHubState();
    recycleIdleTasks();
    pumpQueue();
}

void JZAgentCli::recycleIdleTasks()
{
    const int keep = m_config.task.keepAlive;
    const int64_t maxIdle = (int64_t)m_config.task.idleRecycleSec * 1000;
    const int64_t now = JZAgentCliTask::nowMs();
    int idle = 0;
    for (auto it = m_tasks.begin(); it != m_tasks.end(); )
    {
        JZAgentCliTask *t = *it;
        if (t->busy() || !t->taskId().empty())
        {
            ++it;
            continue;
        }
        ++idle;
        if (idle > keep && t->idleSinceMs() > 0 && now - t->idleSinceMs() >= maxIdle)
        {
            it = m_tasks.erase(it);
            t->deleteLater();
            agentLog("回收空闲任务对象(剩余 " + std::to_string(m_tasks.size()) + " 个)");
            continue;
        }
        ++it;
    }
}

int JZAgentCli::idleWorker() const
{
    for (size_t i = 0; i < m_tasks.size(); ++i)
        if (!m_tasks[i]->busy() && m_tasks[i]->taskId().empty())
            return (int)i;
    return -1;
}

void JZAgentCli::dispatchTo(JZAgentCliTask *task, const Json::Value &req,
                            const std::string &taskId,
                            std::function<void(const Json::Value &)> respond)
{
    task->setTaskId(taskId);
    task->markBusy();
    // 任务对象是"一次性"的: 回完包就销毁(deleteLater, 不会在回调栈里析构)。
    // 注意必须先去 taskId 再销毁 —— 中间不要再碰 task。
    std::function<void(const Json::Value &)> cb = [this, task, respond](const Json::Value &res)
    {
        task->setTaskId("");
        if (respond)
            respond(res);
        releaseTask(task);
        syncHubState();
    };
    task->runTask(req, cb);
    syncHubState();
}

void JZAgentCli::pumpQueue()
{
    if (m_cancelling || m_pumping)
        return;
    m_pumping = true;
    struct PumpGuard
    {
        bool &flag;
        ~PumpGuard() { flag = false; }
    } guard{m_pumping};
    while (!m_pending.empty())
    {
        std::string error;
        JZAgentCliTask *t = acquireTask(m_pending.front().rootDir, error);
        if (!t)
            break;
        QueuedTask q = m_pending.front();
        m_pending.pop_front();
        jzInfo() << "派发排队任务(剩 " << m_pending.size() << " 条)";
        dispatchTo(t, q.req, q.taskId, q.respond);
    }
}

void JZAgentCli::submitTask(const Json::Value &req, std::function<void(const Json::Value &)> respond)
{
    const std::string taskId = m_currentTaskId;
    // 这条任务的工作目录(网页上切换目录后由任务自己带着, 不改进程配置)
    std::string rootError;
    const std::string root = resolveRootDir(req, rootError);
    if (root.empty())
    {
        Json::Value e;
        e["error"] = rootError;
        if (respond) respond(e);
        return;
    }
    if (m_pending.empty())
    {
        std::string error;
        JZAgentCliTask *t = acquireTask(root, error);
        if (t)
        {
            dispatchTo(t, req, taskId, respond);
            return;
        }
        if (!error.empty())
        {
            Json::Value e;
            e["error"] = error;
            if (respond) respond(e);
            return;
        }
    }

    const int cap = m_config.task.maxPending;
    if (cap <= 0 || (int)m_pending.size() >= cap)
    {
        Json::Value e;
        e["error"] = "所有任务都在跑, 且排队已满(" + std::to_string(m_pending.size()) + "/" +
                     std::to_string(cap) + "), 稍后重试";
        e["queue_full"] = true;
        e["queues"] = (int)m_tasks.size();
        if (respond) respond(e);
        return;
    }
    QueuedTask q;
    q.req = req;
    q.taskId = taskId;
    q.rootDir = root;
    q.respond = respond;
    m_pending.push_back(q);
    jzInfo() << "任务排队: " << m_pending.size() << "/" << cap;
}

void JZAgentCli::onWorkerEvent(int q, const std::string &type, const Json::Value &data)
{
    Json::Value d = data;
    if (d.isObject())
        d["queue"] = q;
    emitLocalEvent(type, d);
    const std::string tid = (q >= 0 && q < (int)m_tasks.size()) ? m_tasks[q]->taskId() : std::string();
    if (m_hub && !tid.empty())
        m_hub->sendTaskEvent(tid, type, data);
}

Json::Value JZAgentCli::queuesJson() const
{
    Json::Value arr(Json::arrayValue);
    for (size_t i = 0; i < m_tasks.size(); ++i)
    {
        Json::Value q;
        q["queue"] = (int)i;
        q["busy"] = m_tasks[i]->busy();
        q["round"] = 0;
        q["taskId"] = m_tasks[i]->taskId();
        q["idleMs"] = (Json::Int64)m_tasks[i]->idleSinceMs();
        arr.append(q);
    }
    return arr;
}

bool JZAgentCli::anyBusy() const
{
    for (JZAgentCliTask *t : m_tasks)
        if (t->busy())
            return true;
    return false;
}

int JZAgentCli::pendingCount() const
{
    return (int)m_pending.size();
}

// ---- 任务 -> 宿主的事件出口(任务对象只回调这里, 不自己持有 SSE / hub 订阅者) ----
void JZAgentCli::onTaskReply(JZAgentCliTask *task, const std::string &text)
{
    Json::Value d; d["text"] = text;
    task->pushEvent("reply", d);
}

void JZAgentCli::onTaskThinking(JZAgentCliTask *task, const std::string &text)
{
    Json::Value d; d["text"] = text;
    task->pushEvent("thinking", d);
}

void JZAgentCli::onTaskToolCall(JZAgentCliTask *task, const ToolCall &call)
{
    Json::Value d; d["id"] = call.id; d["name"] = call.function_name; d["args"] = call.function_args;
    task->pushEvent("tool_call", d);
}

void JZAgentCli::onTaskToolResult(JZAgentCliTask *task, const ToolCallResult &result)
{
    Json::Value d; d["id"] = result.id; d["ok"] = result.isOk; d["result"] = result.result;
    task->pushEvent("tool_result", d);
}

void JZAgentCli::onTaskError(JZAgentCliTask *task, const std::string &err)
{
    Json::Value d; d["error"] = err;
    task->pushEvent("error", d);
}

void JZCliToolHost::refresh()
{
    m_terminalConfig.zone.workspace = m_config ? m_config->rootDir : string();
    m_terminalConfig.zone.trustedDirs = m_config ? m_config->trustedDirs : vector<TrustedDir>();
    if (m_config)
    {
        // 档位 = 预设: 先展开成字段, 再用显式配置的单项覆盖
        m_terminalConfig.applyProfile(m_config->profile);
        if (m_config->hasAllowPackageManager)
            m_terminalConfig.allowPackageManager = m_config->allowPackageManager;
        if (m_config->hasAllowNetwork)
            m_terminalConfig.allowNetwork = m_config->allowNetwork;
        if (m_config->hasAllowUnknownPrograms)
            m_terminalConfig.allowUnknownPrograms = m_config->allowUnknownPrograms;
        m_terminalConfig.programPaths = m_config->programPaths;
        m_terminalConfig.env = m_config->env;
    }
}

bool JZCliToolHost::toolReadFile(const std::string &absPath, std::string &text, std::string &error)
{
    JZFileInfo info(absPath);
    if (!info.exists())
    {
        error = "文件不存在: " + absPath;
        return false;
    }
    if (info.isDir())
    {
        error = "这是一个目录(用 list_dir): " + absPath;
        return false;
    }
    if (info.size() > kReadFileLimit)
    {
        error = "文件过大(>8MB), 读不了: " + absPath + "  可以改用终端里的命令按需处理";
        return false;
    }

    JZFile file;
    if (!file.open(absPath, "rb"))
    {
        error = "打开文件失败(权限?): " + absPath;
        return false;
    }
    JZByteArray data = file.readAll();
    file.close();

    text.assign(data.data(), data.size());
    return true;
}

// ==========================================================================
// JZAgentCli(执行体; 宿主 JZAgentCli 见 JZAgentCli.h 末尾)
// ==========================================================================

JZAgentCli::JZAgentCli(JZObject *parent)
    : JZObject(parent)
    
{
}

JZAgentCli::~JZAgentCli()
{
}

void JZAgentCli::startNodes()
{
    if (m_config.nodes.empty())
        return;

    // 包类型要两端一致: 不注册的话收到的 execResult 会被当成 unknown packet 丢掉
    JZAgentNodePacketInit();

    for (const JZCliNodeConfig &nc : m_config.nodes)
    {
        JZNodeLink::Config cfg;
        cfg.name = nc.name;
        cfg.host = nc.host;
        cfg.port = nc.port;
        cfg.token = nc.token;

        auto *link = new JZNodeLink(cfg, m_config.agentName, this);
        m_nodeLinks.push_back(link);
        link->connectToNode();
    }
}

void JZAgentCli::cancelNodeExec(const void *owner)
{
    if (m_nodeCancel)
    {
        m_nodeCancel(owner);
        return;
    }
    for (JZNodeLink *link : m_nodeLinks)
        if (link)
            link->cancelRunning(owner);
}

// ---------------------------------------------------------------------------
// 控制面(managed 模式): 连 server
// ---------------------------------------------------------------------------

void JZAgentCli::startServerLink()
{
    if (!m_config.managed())
        return;

    JZAgentHubPacketInit();

    JZHubLink::Config cfg;
    cfg.agentId = m_config.server.agentId.empty() ? m_config.agentName : m_config.server.agentId;
    cfg.name = m_config.agentName;
    cfg.version = m_config.version;
    cfg.rootDir = m_config.rootDir;
    cfg.project = m_config.rootDir;      // project 就是工作区根目录
    cfg.modelName = m_config.modelName;
    cfg.host = m_config.server.host;
    cfg.port = m_config.server.port;
    cfg.token = m_config.server.token;

    m_hub = new JZHubLink(cfg, this);
    m_hub->setNodesProvider([this]() { return nodesJsonString(); });
    m_hub->setTaskSubmitHandler([this](const std::string &taskId, const Json::Value &body)
    {
        onHubTaskSubmit(taskId, body);
    });
    m_hub->setTaskCancelHandler([this](const std::string &taskId) { onHubTaskCancel(taskId); });
    m_hub->setResetHandler([this](const std::string &scope) { onHubReset(scope); });
    m_hub->setStateHandler([this]()
    {
        if (!m_hub)
            return;
        if (m_hub->online())
            jzInfo().noquote().nospace() << "控制面就绪(受 server 调度): " << m_config.server.host
                                         << ":" << m_config.server.port;
        else if (!m_hub->lastError().empty())
            jzWarning().noquote().nospace() << "控制面不可用: " << m_hub->lastError();
    });
    m_hub->connectToServer();
}

bool JZAgentCli::hubOnline() const
{
    return m_hub && m_hub->online();
}

void JZAgentCli::syncHubState()
{
    if (!m_hub)
        return;

    int round = 0;
    std::string taskId;
    for (size_t i = 0; i < m_tasks.size(); ++i)
    {
        if (!m_tasks[i]->m_busy)
            continue;
        round = m_tasks[i]->m_loopRound;
        taskId = m_tasks[i]->taskId();
        break;  // 协议仍是单 currentTask; busy 必须按所有队列聚合, 展示第一个在跑任务
    }
    m_hub->setTaskState(anyBusy(), round, taskId);
}

void JZAgentCli::onHubTaskSubmit(const std::string &taskId, const Json::Value &body)
{
    // 提交统一走 submitTask 调度: 有空闲队列立即跑, 全忙进 FIFO 排队(maxPending 上限)。
    // ⚠ submitTask 的"排队已满/参数错"是**同步**回调的, 那时在跑的任务还没结束 ——
    //   所以失败时要把 m_currentTaskId 恢复成上一个值, 否则在跑任务的事件会丢 id。
    const std::string prev = m_currentTaskId;
    m_currentTaskId = taskId;

    const std::string tid = taskId;
    submitTask(body, [this, tid, prev](const Json::Value &res)
    {
        if (m_hub)
            m_hub->sendTaskEnd(tid, res);

        const bool earlyFail = !res.isMember("status");   // 同步失败只带 error
        m_currentTaskId = earlyFail ? prev : std::string();
        syncHubState();
    });
}

void JZAgentCli::onHubTaskCancel(const std::string &taskId)
{
    if (false)
    {
        if (m_currentTaskId.empty() || (!taskId.empty() && taskId != m_currentTaskId))
            return;
        cancelTask("任务已被 server 取消");
        return;
    }

    // master: 先看还在排队的(直接摘掉并回 cancelled, 不浪费队列)
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
    {
        if (!taskId.empty() && it->taskId != taskId)
            continue;
        QueuedTask t = *it;
        m_pending.erase(it);
        Json::Value r;
        r["status"] = "cancelled";
        r["verify_rounds"] = 0;
        r["agent_reply"] = "";
        r["last_verify_output"] = "";
        r["error"] = "任务在排队中被取消";
        if (t.respond)
            t.respond(r);   // managed 的 respond 已负责发送 taskEnd, 不能再发第二次
        return;
    }

    // 再看各队列在跑的: 即使命中 q0 也只能停 q0, 不能走 master 的全局 reset
    for (size_t i = 0; i < m_tasks.size(); ++i)
    {
        const std::string cur = m_tasks[i]->taskId();
        if (!taskId.empty() && cur != taskId)
            continue;
        if (cur.empty() && !m_tasks[i]->m_busy)
            continue;
        m_tasks[i]->cancelLocalTask("任务已被 server 取消");
        return;
    }
}

void JZAgentCli::onHubReset(const std::string &scope)
{
    if (scope == "task")
        cancelTask("任务已被 server 重置");
    else
        resetHistory();
}

Json::Value JZAgentCli::nodesJson() const
{
    // 节点状态只有一个出口: /info、控制面 hello/心跳、管理台都取这里, 免得三处口径不一。
    Json::Value nodes(Json::arrayValue);
    for (JZNodeLink *link : m_nodeLinks)
    {
        Json::Value item(Json::objectValue);
        item["name"] = link->config().name;
        item["target"] = link->config().host + ":" + std::to_string(link->config().port);
        item["online"] = link->online();
        item["busy"] = link->busy();
        item["nodeName"] = link->nodeName();
        item["rootDir"] = link->rootDir();
        item["shell"] = link->shell();
        item["error"] = link->lastError();
        nodes.append(item);
    }
    return nodes;
}

std::string JZAgentCli::nodesJsonString() const
{
    std::string s = Json::FastWriter().write(nodesJson());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

JZNodeLink *JZAgentCli::nodeLink(const std::string &name) const
{
    if (name.empty())
        return nullptr;   // 没有"默认节点"这回事: host 必填
    for (JZNodeLink *link : m_nodeLinks)
    {
        if (link->config().name == name)
            return link;
    }
    return nullptr;
}

void JZAgentCli::startHttp()
{
    m_http = new JZHttpServer(this);

    // 网页控制台: / 直接是对话页(agent.html), 其余静态资源从 httpDir 取。
    //   ⚠ 页面(app.js/app.css/agent.html)和 server 管理台是**同一份**, 不再各写一套。
    //   默认目录 <exeDir>/webui (源目录 binary/agent_cli/webui)。
    std::string httpDir = m_config.httpDir;
    if (httpDir.empty())
        httpDir = JZPlatform::applicationDirPath() + "/webui";
    auto *staticFiles = new JZHttpStaticFiles(httpDir);
    m_http->addRoute("/", new DashboardHandler(this));
    m_http->addRoute("/<path:path>", staticFiles);

    // A2A 网页接口: 与 server 管理台同一套, 前端共用一份页面
    m_http->addRoute("/api/tasks", new ApiTasksHandler(this));
    m_http->addRoute("/api/tasks/detail", new ApiTaskDetailHandler(this));
    m_http->addRoute("/api/tasks/cancel", new ApiTaskCancelHandler(this));
    m_http->addRoute("/api/tasks/delete", new ApiTaskDeleteHandler(this));
    m_http->addRoute("/api/tasks/subscribe", new ApiTaskSubscribeHandler(this));
    m_http->addRoute("/api/agent", new ApiAgentHandler(this));
    m_http->addRoute("/api/status", new ApiStatusHandler(this));
    // 终端面板也统一到 /api 前缀下(server 没有这两个接口 -> 网页自动隐藏该块)
    m_http->addRoute("/api/terminal", new TerminalListHandler(this));
    m_http->addRoute("/api/terminal/cancel", new TerminalCancelHandler(this));

    // 供网页下拉选择验证脚本
    m_http->addRoute("/scripts", new ScriptsHandler(this));

    // 网页顶栏显示: 模型地址 / 模型名 / 工作区
    m_http->addRoute("/info", new InfoHandler(this));

    m_http->addRoute("/task", new TaskHandler(this));
    m_http->addRoute("/reset", new ResetHandler(this));
    m_http->addRoute("/events", new EventsHandler(this));

    // 后台任务面板(网页): 列运行 / 取日志 / 停一条 —— 人想"看看服务在打什么"时不用去问模型
    m_http->addRoute("/terminal", new TerminalListHandler(this));
    m_http->addRoute("/terminal/cancel", new TerminalCancelHandler(this));
    if (!m_http->listen(m_config.httpBind, m_config.httpPort))
        jzWarning() << "HttpServer 监听失败:" << m_config.httpBind << ":" << m_config.httpPort;
}


// ==========================================================================
// MCP 客户端(把外部 MCP 服务器的工具挂进本 agent)
//   配置驱动: 加减 server 只改 config.json(不用重编译); 启动时逐个连, 连上才登记工具。
//   两条必须知道的约束(详见 JZAgentCli.h 的 JZCliMcpServer):
//     ① 挂载只能在 Inactive(首个任务之前): JZAgent 拒绝在会话开始后改工具表
//        (会破坏 KV 前缀缓存, 见 rejectWhileActive) —— 晚到的连接只记警告;
//     ② 远端工具在对端执行, 不受本机 terminal 的区域/路径审核约束。
// ==========================================================================

void JZAgentCli::startMcp()
{
    if (!m_config.mcpEnabled || m_config.mcpServers.empty())
    {
        m_mcpInitDone = true;   // 没配 MCP: 不挡任务
        return;
    }

    m_mcpStates.clear();
    m_mcpStates.reserve(m_config.mcpServers.size());

    // 闸门: 从这一刻起新的 POST /task 先等着 —— 等下面每个连接都有结果(连上 / 连不上)。
    // 为什么必须等: JZAgent 只允许在 Inactive(首个任务之前)改工具表, 任务先跑起来以后
    // 连上的远端工具就挂不进去了(只能重启进程), 那还不如让提交方稍等几秒。
    m_mcpInitDone = false;
    m_mcpPending = 0;

    for (size_t i = 0; i < m_config.mcpServers.size(); i++)
    {
        const JZCliMcpServer &cfg = m_config.mcpServers[i];
        const std::string transport = str_toLower(cfg.transport);

        // 每条配置都先记一条状态(哪怕连不起来), 好让 /info 与日志能对账
        JZCliMcpState st;
        st.name = cfg.name.empty() ? transport : cfg.name;
        st.transport = transport;
        if (transport == "http")
            st.target = cfg.url;
        else
        {
            st.target = cfg.command;
            for (size_t k = 0; k < cfg.args.size(); k++)
                st.target += " " + cfg.args[k];
        }
        m_mcpStates.push_back(st);
        JZCliMcpState &state = m_mcpStates.back();

        // 配置校验在 loadConfig 已做过一遍, 这里再挡一次(防别的入口塞进来的配置)
        if (transport != "stdio" && transport != "http")
        {
            state.error = "transport 不支持(要用 stdio 或 http): " + cfg.transport;
            jzWarning() << "MCP server" << state.name << state.error;
            continue;
        }
        if (transport == "stdio" && cfg.command.empty())
        {
            state.error = "stdio 缺 command";
            jzWarning() << "MCP server" << state.name << state.error;
            continue;
        }
        if (transport == "http" && cfg.url.empty())
        {
            state.error = "http 缺 url";
            jzWarning() << "MCP server" << state.name << state.error;
            continue;
        }

        // 不给 parent: addMcp 要求接管时 client 还没有 parent(见 JZAgent::addMcp)。
        auto *client = new JZMcpClient();
        if (!cfg.toolPrefix.empty())
            client->setToolPrefix(cfg.toolPrefix);
        if (cfg.callTimeoutMs > 0)
            client->setCallTimeout(cfg.callTimeoutMs);

        // 先接好信号再发起连接(连接是异步的, 结束统一走 sigFinished)
        const int index = (int)i;
        connect(&client->sigFinished, [this, client, index]() { onMcpFinished(client, index); });

        const bool started = (transport == "http")
            ? client->startHttp(cfg.url, cfg.timeoutMs)
            : client->startProcess(cfg.command, cfg.args, cfg.timeoutMs);
        if (!started)
        {
            state.error = "发起连接失败: " + client->errorString();
            jzWarning() << "MCP" << state.name << state.error;
            client->deleteLater();
            continue;
        }
        m_mcpPending++;
        jzInfo() << "MCP 连接中:" << state.name
                 << (transport == "http" ? cfg.url : state.target);
    }

    // 一条都没发起成功(纯配置错误): 没有在途连接, 立刻放行, 别把任务卡死
    if (m_mcpPending == 0)
        m_mcpInitDone = true;
    else
        jzInfo() << "MCP 初始化中:" << m_mcpPending << " 个连接 —— 完成前提交任务会被拒绝";
}

void JZAgentCli::onMcpFinished(JZMcpClient *client, int index)
{
    // ⚠ 本函数**每个出口**都要走 finishMcpPending() 记账 —— 漏一个, 闸门就永远开不了。
    JZCliMcpState *state = (index >= 0 && index < (int)m_mcpStates.size())
                               ? &m_mcpStates[index] : nullptr;
    const std::string name = (state && !state->name.empty()) ? state->name : std::string("?");

    if (!client->isReady())
    {
        if (state)
            state->error = "连接失败: " + client->errorString();
        jzWarning() << "MCP 连接失败:" << name << client->errorString();
        client->deleteLater();
        finishMcpPending();
        return;
    }

    // addMcp 在 agent 非 Inactive 时会直接拒绝(只打 warning), 用 parent 判断是否真的接管了。
    JZAgent *agent = m_tasks.empty() ? nullptr : m_tasks.front()->agent();
    if (!agent)
    {
        m_mcpClients.push_back(client);   // 任务对象还没建, 等首次提交时挂
        finishMcpPending();
        return;
    }
    agent->addMcp(client);
    if (client->parent() != agent)
    {
        if (state)
            state->error = "挂载被拒: agent 已经有会话在跑(改工具表会作废 KV 前缀缓存), 需重启进程";
        jzWarning() << "MCP 工具挂载被拒:" << name
                    << " —— agent 已有会话在跑, 这个 server 的工具要重启进程才会生效";
        client->deleteLater();
        finishMcpPending();
        return;
    }

    std::string tools;
    for (const JZAgentTool *tool : client->tools())
    {
        if (!tools.empty())
            tools += ", ";
        tools += tool->toolName();
    }
    if (state)
    {
        state->ready = true;
        state->tools = (int)client->tools().size();
    }
    jzInfo() << "MCP 已挂载:" << client->serverName() << client->serverVersion()
             << "  工具数:" << (int)client->tools().size()
             << (tools.empty() ? std::string() : ("  [" + tools + "]"));
    finishMcpPending();
}

// 一个 MCP 连接流程结束了: 未结束数 -1; 减到 0 = 配置里的 server 全部有结果(连上或连不上),
// 此时放行新任务(见 submitTask 的 m_mcpInitDone 检查)。

void JZAgentCli::finishMcpPending()
{
    if (m_mcpPending > 0)
        m_mcpPending--;
    if (m_mcpPending == 0 && !m_mcpInitDone)
    {
        m_mcpInitDone = true;
        jzInfo() << "MCP 初始化完毕 —— 现在可以提交任务了";
    }
}

Json::Value JZAgentCli::makeInfo() const
{
    Json::Value j;
    j["agent"] = m_config.agentName;
    j["version"] = m_config.version;
    j["modelUrl"] = m_config.modelUrl;
    j["modelName"] = m_config.modelName;
    j["rootDir"] = m_config.rootDir;
    j["mcpInitDone"] = m_mcpInitDone;   // false = 还在连 MCP, 此时的 POST /task 会被拒绝
    // 任务队列: 每个队列的忙闲/轮次/当前任务(queues==1 时数组只有一项, 与旧版等价)
    j["queues"] = queuesJson();
    j["pendingTasks"] = pendingCount();
    // 宿主常驻说明(system.extra / extraFile): 拼进 system prompt 一次, 之后每轮 /task 只叠加新消息 ——
    // 这里给出"从哪来 / 多大 / system prompt 总共多大", 便于自检(HTTP 模式默认不打 stdout)
    j["systemExtraFile"] = m_config.systemExtraFile;
    j["systemExtraBytes"] = (int)m_config.systemExtra.size();
    j["systemPromptBytes"] = m_tasks.empty() ? 0 : m_tasks.front()->m_systemPromptBytes;
    // MCP: 配置里每个 server 的连接/挂载状态(没有网页面板时用 GET /info 自检)
    if (!m_mcpStates.empty())
    {
        Json::Value mcp(Json::arrayValue);
        for (const JZCliMcpState &s : m_mcpStates)
        {
            Json::Value item(Json::objectValue);
            item["name"] = s.name;
            item["transport"] = s.transport;
            item["target"] = s.target;
            item["ready"] = s.ready;
            item["tools"] = s.tools;
            item["error"] = s.error;
            mcp.append(item);
        }
        j["mcp"] = mcp;
    }

    // 远端节点(link 模式 agent_node): 在线状态 + 节点自述。
    // 没有网页面板、控制台日志又是缓冲的时候, `curl /info` 是唯一能确认
    // "hello/helloAck 到底成没成、token 是否配错"的自检口子。
    if (!m_config.nodes.empty())
        j["nodes"] = nodesJson();

    // 控制面(managed): 这台 cli 有没有被 server 接管 —— managed 下没有 HTTP,
    // 这份信息主要经心跳回给 server 的管理台, 这里留着便于 --check 之外的现场自检。
    if (m_config.managed())
    {
        Json::Value hub(Json::objectValue);
        hub["target"] = m_config.server.host + ":" + std::to_string(m_config.server.port);
        hub["agentId"] = m_config.server.agentId.empty() ? m_config.agentName : m_config.server.agentId;
        hub["online"] = hubOnline();
        hub["error"] = m_hub ? m_hub->lastError() : std::string("控制面未启动");
        j["hub"] = hub;
    }
    return j;   // 注意: 不含 modelToken, 别把凭据吐给网页
}

void JZAgentCli::onInfo(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req))
    {
        sendJson(client, 401, Json::objectValue);
        return;
    }

    Json::Value j = makeInfo();
    j["busy"] = anyBusy();           // 多队列: 任一队列在跑就算忙
    j["round"] = m_tasks.empty() ? 0 : m_tasks.front()->m_loopRound;
    j["recent"] = recentJson();      // 最近若干次请求与结果(网页刚打开时靠它补回历史)
    sendJson(client, 200, j);
}

void JZAgentCli::onTerminalList(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req))
    {
        sendJson(client, 401, Json::objectValue);
        return;
    }

    // 单前台任务: 没有"运行记录"可列, 只回当前状态(网页那一栏据此显示是否在跑)
    Json::Value r;
    r["running"] = (m_terminal && m_terminal->isRunning()) ? 1 : 0;
    r["runs"] = Json::Value(Json::arrayValue);
    if (m_terminal)
    {
        r["session"] = m_terminal->id();
        std::string cwd;
        if (m_terminal->printDir(cwd))
            r["cwd"] = cwd;
    }
    sendJson(client, 200, r);
}

void JZAgentCli::onTerminalCancel(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req))
    {
        sendJson(client, 401, Json::objectValue);
        return;
    }

    (void)req;
    bool ok = false;
    int killed = 0;
    if (m_terminal)
    {
        killed = m_terminal->cancel() ? 1 : 0;   // 单前台任务: 停的就是当前那条
        ok = (killed > 0);
    }

    Json::Value r;
    r["ok"] = ok;
    r["killed"] = killed;
    if (ok)
    {
        Json::Value e;
        e["killed"] = killed;
        pushEvent("terminal_cancel", e);   // 其它开着的页面也刷新一下
    }
    sendJson(client, ok ? 200 : 409, r);
}

void JZAgentCli::sendJson(JZHttpServerClient *client, int code, const Json::Value &j)
{
    JZHttpRespone resp;
    resp.setStatusCode(code);
    resp.setJson(Json::FastWriter().write(j));
    client->response(resp.toByteArray());
}

// ==========================================================================
// 实时事件流(GET /events, SSE) —— 任务进行中的即时反馈
// ==========================================================================

// SSE 帧 -> HTTP chunked 分块(长度前缀 + 帧 + CRLF)。
// /events 声明了 Transfer-Encoding: chunked, 每帧必须按分块格式写, 否则客户端解不出帧边界。
static std::string makeSseChunk(const std::string &payload)
{
    std::string frame = "data: " + payload + "\n\n";
    char head[32];
    snprintf(head, sizeof(head), "%x\r\n", (unsigned)frame.size());
    std::string chunk = head;
    chunk += frame;
    chunk += "\r\n";
    return chunk;
}

void JZAgentCli::onEvents(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req))
    {
        sendJson(client, 401, Json::objectValue);
        return;
    }

    // 响应头不带 Content-Length; 用 chunked 分块 —— 浏览器 EventSource 两种都能实时收,
    // 但只有 chunked 才能让**非浏览器**客户端(如 JZNetworkReply)逐帧读到: 无 Content-Length
    // 的流式响应在那些客户端里要等连接关闭才一次性交付, 过程就全憋到最后了。
    JZHttpRespone resp;
    resp.setStatusCode(200);
    resp.setContentType("text/event-stream");
    resp.setHeader("Cache-Control", "no-cache");
    resp.setHeader("Connection", "keep-alive");
    resp.setHeader("X-Accel-Buffering", "no");
    resp.setHeader("Transfer-Encoding", "chunked");
    client->write(resp.toHeadByteArray(0, false), false);

    m_eventClients.push_back(client);
    connect(&client->sigDestroyed, this, [this, client](JZObject *) {
        removeEventClient(client);
    });

    Json::Value d = makeInfo();
    d["busy"] = anyBusy();
    d["round"] = m_tasks.empty() ? 0 : m_tasks.front()->m_loopRound;
    // 最近结果也按所有队列挑选, 不能只看 q0。
    if (!anyBusy())
    {
        const JZAgentCliTask *latest = nullptr;
        for (const JZAgentCliTask *w : m_tasks)
        {
            if (!w->m_hasLastResult || w->m_history.empty())
                continue;
            if (!latest || w->m_history.back().endMs > latest->m_history.back().endMs)
                latest = w;
        }
        if (latest)
            d["last_result"] = latest->m_lastResult;
    }
    d["recent"] = recentJson();      // 最近若干次请求与结果: 新开的网页靠它把历史卡片补回来
    Json::Value first;
    first["type"] = "hello";
    first["data"] = d;
    std::string payload = Json::FastWriter().write(first);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();
    std::string chunk = makeSseChunk(payload);
    client->write(chunk.data(), (int)chunk.size(), false);

    // 每个在跑队列各推一份快照, queue 字段让界面不会把不同会话拼在一起。
    const size_t kSnapCap = 6000;
    auto tail = [&](const std::string &s) -> std::string {
        return s.size() <= kSnapCap ? s : ("…(前面已省略)\n" + s.substr(s.size() - kSnapCap));
    };
    for (size_t i = 0; i < m_tasks.size(); ++i)
    {
        const JZAgentCliTask *w = m_tasks[i];
        if (!w->m_busy)
            continue;

        Json::Value s;
        s["queue"] = (int)i;
        s["busy"] = true;
        s["round"] = w->m_loopRound;
        s["verify_cmd"] = w->m_verifyCmd;
        s["prompt"] = w->m_currentPrompt;
        s["reply"] = tail(w->m_accumReply);
        s["think"] = tail(w->m_accumThink);
        s["last_verify_output"] = w->m_lastVerifyOutput;
        Json::Value tools(Json::arrayValue);
        for (const auto &t : w->m_toolSnaps)
        {
            Json::Value o;
            o["id"] = t.id;
            o["name"] = t.name;
            o["args"] = t.args;
            if (t.hasResult)
            {
                o["ok"] = t.ok;
                o["result"] = t.result;
            }
            tools.append(o);
        }
        s["tools"] = tools;
        emitLocalEvent("state", s);
    }
}

// ==========================================================================
// A2A 网页接口 —— 与 server 管理台同一套 /api/*, 前端共用一份页面
//   内部任务模型不变: 这里只是把"当前任务 + 最近历史"翻译成 A2A Task。
// ==========================================================================
static long long a2aNowMs()
{
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string a2aJsonLine(const Json::Value &v)
{
    std::string s = Json::FastWriter().write(v);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

static std::string a2aSseChunk(const std::string &event, const std::string &payload)
{
    std::string frame = "event: " + event + "\n";
    frame += "data: " + payload + "\n\n";
    char head[32];
    snprintf(head, sizeof(head), "%x\r\n", (unsigned)frame.size());
    std::string chunk(head);
    chunk += frame;
    chunk += "\r\n";
    return chunk;
}

static const char *kA2aSubmitted = "TASK_STATE_SUBMITTED";
static const char *kA2aWorking = "TASK_STATE_WORKING";
static const char *kA2aCompleted = "TASK_STATE_COMPLETED";
static const char *kA2aFailed = "TASK_STATE_FAILED";
static const char *kA2aCanceled = "TASK_STATE_CANCELED";
static const char *kA2aUnknown = "TASK_STATE_UNSPECIFIED";

static bool a2aIsWorking(const std::string &s)
{
    return s == kA2aSubmitted || s == kA2aWorking;
}

static std::string a2aStateFromStatus(const std::string &st)
{
    if (st == "success") return kA2aCompleted;
    if (st == "cancelled") return kA2aCanceled;
    if (st == "running") return kA2aWorking;
    return kA2aFailed;
}

// A2A SendMessage 的正文: message.parts[].text, 兼容老的 prompt 字段
static std::string a2aMessageText(const Json::Value &body)
{
    if (body.isMember("prompt") && body["prompt"].isString())
        return body["prompt"].asString();
    const Json::Value &parts = body["message"]["parts"];
    if (!parts.isArray())
        return std::string();
    std::string out;
    for (Json::ArrayIndex i = 0; i < parts.size(); ++i)
    {
        const std::string t = parts[i].get("text", "").asString();
        if (t.empty())
            continue;
        if (!out.empty())
            out += "\n";
        out += t;
    }
    return out;
}

std::string JZAgentCli::a2aAgentId() const
{
    if (!m_config.server.agentId.empty())
        return m_config.server.agentId;   // managed 时 server 派下来的 id
    if (!m_config.agentName.empty())
        return m_config.agentName;
    return std::string("agent_cli");
}

Json::Value JZAgentCli::a2aTaskJson(const A2aTask &t) const
{
    Json::Value j(Json::objectValue);
    j["id"] = t.id;
    j["contextId"] = "ctx-" + a2aAgentId();   // 单 agent: 所有任务同一条会话

    Json::Value st(Json::objectValue);
    st["state"] = t.state;
    if (!t.error.empty())
    {
        Json::Value msg(Json::objectValue);
        msg["messageId"] = t.id + ":status";
        msg["role"] = "ROLE_AGENT";
        Json::Value parts(Json::arrayValue);
        Json::Value part(Json::objectValue);
        part["text"] = t.error;
        parts.append(part);
        msg["parts"] = parts;
        st["message"] = msg;
    }
    j["status"] = st;

    Json::Value arts(Json::arrayValue);
    if (!t.reply.empty())
    {
        Json::Value a(Json::objectValue);
        a["artifactId"] = "reply";
        a["name"] = "agent 回复";
        Json::Value parts(Json::arrayValue);
        Json::Value part(Json::objectValue);
        part["text"] = t.reply;
        parts.append(part);
        a["parts"] = parts;
        arts.append(a);
    }
    if (!t.verifyOut.empty())
    {
        Json::Value a(Json::objectValue);
        a["artifactId"] = "verify";
        a["name"] = "验证输出";
        Json::Value parts(Json::arrayValue);
        Json::Value part(Json::objectValue);
        part["text"] = t.verifyOut;
        parts.append(part);
        a["parts"] = parts;
        arts.append(a);
    }
    j["artifacts"] = arts;

    Json::Value hist(Json::arrayValue);
    {
        Json::Value m(Json::objectValue);
        m["messageId"] = t.id + ":user";
        m["role"] = "ROLE_USER";
        Json::Value parts(Json::arrayValue);
        Json::Value part(Json::objectValue);
        part["text"] = t.prompt;
        parts.append(part);
        m["parts"] = parts;
        hist.append(m);
    }
    j["history"] = hist;

    Json::Value meta(Json::objectValue);
    meta["agentId"] = a2aAgentId();
    meta["prompt"] = t.prompt;
    meta["verifyCmd"] = t.verifyCmd;
    meta["origin"] = "user";             // cli 侧只有人下发的任务
    meta["rounds"] = t.rounds;
    meta["lastEventType"] = t.events.empty() ? std::string() : t.events.back().get("type", "").asString();
    Json::Value evs(Json::arrayValue);
    for (size_t i = 0; i < t.events.size(); ++i)
        evs.append(t.events[i]);
    meta["events"] = evs;
    meta["eventSeq"] = t.eventSeq;
    meta["startMs"] = (Json::Int64)t.startMs;
    meta["endMs"] = (Json::Int64)t.endMs;
    meta["costMs"] = (Json::Int64)((t.endMs > t.startMs) ? (t.endMs - t.startMs) : 0);
    j["metadata"] = meta;
    return j;
}

JZAgentCli::A2aTask JZAgentCli::a2aTaskFromRecent(const Json::Value &it) const
{
    A2aTask t;
    t.id = "h" + std::to_string(it.get("seq", 0).asInt());
    t.prompt = it.get("prompt", "").asString();
    t.verifyCmd = it.get("verify_cmd", "").asString();
    t.state = a2aStateFromStatus(it.get("status", "").asString());
    t.reply = it.get("reply", "").asString();
    t.verifyOut = it.get("verify_output", "").asString();
    t.error = it.get("error", "").asString();
    t.rounds = it.get("rounds", 0).asInt();
    t.startMs = it.get("start_ms", (Json::Int64)0).asInt64();
    t.endMs = it.get("end_ms", (Json::Int64)0).asInt64();
    return t;
}

Json::Value JZAgentCli::a2aHistoryJson(int limit) const
{
    Json::Value arr(Json::arrayValue);
    const Json::Value recent = recentJson();
    int count = 0;
    for (Json::ArrayIndex i = 0; i < recent.size() && count < limit; ++i)
    {
        // 正在跑的那条由 m_a2aLive 代表(带事件), 历史里跳过, 免得重复
        if (m_a2aLiveActive && recent[i].get("status", "").asString() == "running")
            continue;
        arr.append(a2aTaskJson(a2aTaskFromRecent(recent[i])));
        ++count;
    }
    return arr;
}

Json::Value JZAgentCli::a2aFindTask(const std::string &id, bool &live) const
{
    live = false;
    if (m_a2aLiveActive && m_a2aLive.id == id)
    {
        live = true;
        return a2aTaskJson(m_a2aLive);
    }
    const Json::Value recent = recentJson();
    for (Json::ArrayIndex i = 0; i < recent.size(); ++i)
    {
        const std::string hid = "h" + std::to_string(recent[i].get("seq", 0).asInt());
        if (hid == id)
            return a2aTaskJson(a2aTaskFromRecent(recent[i]));
    }
    return Json::Value(Json::nullValue);
}

void JZAgentCli::a2aEmit(const std::string &type, const Json::Value &data)
{
    if (!m_a2aLiveActive)
        return;
    if (type == "hello" || type == "state" || type == "reset" || type == "task_end")
        return;

    Json::Value e(Json::objectValue);
    e["seq"] = ++m_a2aLive.eventSeq;
    e["at"] = (Json::Int64)a2aNowMs();
    e["type"] = type;
    e["data"] = data;
    m_a2aLive.events.push_back(e);
    if (m_a2aLive.events.size() > 200)
        m_a2aLive.events.erase(m_a2aLive.events.begin());
    if (a2aIsWorking(m_a2aLive.state) && type != "task_start")
        m_a2aLive.state = kA2aWorking;

    const std::string payload = a2aJsonLine(e);
    std::vector<JZHttpServerClient *> targets;
    for (auto &kv : m_a2aSubs)
        if (kv.second == m_a2aLive.id)
            targets.push_back(kv.first);          // 拷贝: write 可能触发断连回调
    for (JZHttpServerClient *c : targets)
    {
        std::string chunk = a2aSseChunk("status_update", payload);
        c->write(chunk.data(), (int)chunk.size(), false);
    }
}

void JZAgentCli::a2aFinish(const Json::Value &result)
{
    if (!m_a2aLiveActive)
        return;
    const std::string st = result.get("status", "").asString();
    m_a2aLive.state = a2aStateFromStatus(st);
    m_a2aLive.reply = result.get("agent_reply", "").asString();
    m_a2aLive.verifyOut = result.get("last_verify_output", "").asString();
    m_a2aLive.error = result.get("error", "").asString();
    m_a2aLive.rounds = result.get("verify_rounds", 0).asInt();
    m_a2aLive.endMs = a2aNowMs();

    const std::string id = m_a2aLive.id;
    const std::string payload = a2aJsonLine(a2aTaskJson(m_a2aLive));
    std::vector<JZHttpServerClient *> targets;
    for (auto &kv : m_a2aSubs)
        if (kv.second == id)
            targets.push_back(kv.first);
    for (JZHttpServerClient *c : targets)
    {
        std::string chunk = a2aSseChunk("task", payload);
        c->write(chunk.data(), (int)chunk.size(), false);
        c->write("0\r\n\r\n", 5, true);           // 终态关流
    }
    for (JZHttpServerClient *c : targets)
        m_a2aSubs.erase(c);

    m_a2aLiveActive = false;   // 之后由"最近历史"承接这条
}

void JZAgentCli::onApiTasks(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req))
    {
        sendJson(client, 401, Json::objectValue);
        return;
    }

    if (req.method() == JZHttpRequest::POST)
    {
        Json::Value body;
        Json::Reader reader;
        if (!reader.parse(std::string(req.body().data(), req.body().size()), body) || !body.isObject())
        {
            Json::Value e; e["error"] = "body 必须是 JSON 对象";
            sendJson(client, 400, e);
            return;
        }
        const std::string prompt = a2aMessageText(body);
        if (prompt.empty())
        {
            Json::Value e; e["error"] = "缺少 message.parts[].text";
            sendJson(client, 400, e);
            return;
        }
        if (m_a2aLiveActive || anyBusy())
        {
            Json::Value e;
            e["error"] = "agent 正忙, 等当前任务结束再发";
            e["taskId"] = m_a2aLiveActive ? m_a2aLive.id : std::string();
            sendJson(client, 409, e);
            return;
        }

        const std::string id = "t" + std::to_string(a2aNowMs()) + "-" + std::to_string(++m_a2aSeq);
        m_a2aLive = A2aTask();
        m_a2aLive.id = id;
        m_a2aLive.prompt = prompt;
        m_a2aLive.verifyCmd = body.get("verifyCmd", body.get("verify_cmd", "")).asString();
        m_a2aLive.state = kA2aSubmitted;
        m_a2aLive.startMs = a2aNowMs();
        m_a2aLiveActive = true;

        Json::Value run(Json::objectValue);
        run["prompt"] = prompt;
        if (!m_a2aLive.verifyCmd.empty())
            run["verify_cmd"] = m_a2aLive.verifyCmd;
        if (body.isMember("maxVerifyRounds"))
            run["max_verify_rounds"] = body["maxVerifyRounds"];

        auto early = std::make_shared<Json::Value>();
        auto failed = std::make_shared<bool>(false);
        submitTask(run, [this, id, early, failed](const Json::Value &res)
        {
            if (!res.isMember("status"))
            {
                *early = res;       // 同步拒绝(排队满/参数错)
                *failed = true;
                return;
            }
            if (!m_a2aLiveActive || m_a2aLive.id != id)
                return;             // 已被取消/换了一条
            a2aFinish(res);
        });

        if (*failed)
        {
            m_a2aLiveActive = false;
            const bool full = (*early).get("queue_full", false).asBool();
            Json::Value e;
            e["error"] = (*early).get("error", "提交失败").asString();
            sendJson(client, full ? 429 : 400, e);
            return;
        }
        sendJson(client, 202, a2aTaskJson(m_a2aLive));   // SendMessage 回包就是一个 Task
        return;
    }

    int limit = atoi(req.queryParam("limit").c_str());
    if (limit <= 0 || limit > 500)
        limit = 50;

    Json::Value arr(Json::arrayValue);
    int total = 0;
    if (m_a2aLiveActive)
    {
        arr.append(a2aTaskJson(m_a2aLive));
        ++total;
    }
    const Json::Value recent = a2aHistoryJson(limit);
    for (Json::ArrayIndex i = 0; i < recent.size() && (int)arr.size() < limit; ++i)
        arr.append(recent[i]);
    total += (int)recent.size();

    Json::Value j(Json::objectValue);
    j["tasks"] = arr;
    j["totalSize"] = total;
    j["pageSize"] = limit;
    j["nextPageToken"] = "";
    sendJson(client, 200, j);
}

void JZAgentCli::onApiTaskDetail(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req)) { sendJson(client, 401, Json::objectValue); return; }
    const std::string id = req.queryParam("id");
    bool live = false;
    Json::Value t = a2aFindTask(id, live);
    if (t.isNull())
    {
        Json::Value e; e["error"] = "没有这个 Task: " + id;
        sendJson(client, 404, e);
        return;
    }
    sendJson(client, 200, t);
}

void JZAgentCli::onApiTaskCancel(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req)) { sendJson(client, 401, Json::objectValue); return; }
    const std::string id = req.queryParam("id");
    bool live = false;
    Json::Value t = a2aFindTask(id, live);
    if (t.isNull())
    {
        Json::Value e; e["error"] = "没有这个 Task: " + id;
        sendJson(client, 404, e);
        return;
    }
    if (!live)
    {
        Json::Value e; e["error"] = "这条任务已经结束了";
        sendJson(client, 409, e);
        return;
    }
    cancelTask("任务已取消");
    sendJson(client, 202, t);      // A2A: 只是"已请求取消", 终态走 SSE
}

// 删历史条目: 只删已结束的; 在跑的(id 是 t...)先取消, 不许删。
// POST /api/tasks/delete  body {"id":"h12"} 或 {"ids":["h12","h10"]}; 也接受 ?id=
void JZAgentCli::onApiTaskDelete(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req)) { sendJson(client, 401, Json::objectValue); return; }

    std::vector<std::string> ids;
    const std::string qid = req.queryParam("id");
    if (!qid.empty())
        ids.push_back(qid);

    Json::Value body;
    Json::Reader reader;
    if (reader.parse(std::string(req.body().data(), req.body().size()), body) && body.isObject())
    {
        const Json::Value &jid = body["id"];
        if (jid.isString() && !jid.asString().empty())
            ids.push_back(jid.asString());
        const Json::Value &list = body["ids"];
        if (list.isArray())
            for (Json::ArrayIndex i = 0; i < list.size(); ++i)
                if (list[i].isString())
                    ids.push_back(list[i].asString());
    }
    if (ids.empty())
    {
        Json::Value e; e["error"] = "缺少 id";
        sendJson(client, 400, e);
        return;
    }

    int removed = 0, canceled = 0;
    Json::Value skipped(Json::arrayValue);
    for (size_t i = 0; i < ids.size(); ++i)
    {
        const std::string &id = ids[i];
        if (id.empty())
            continue;
        // 在跑的那条: 先取消, 再把它的"running"历史条目删掉(收尾时不再回填)
        if (m_a2aLiveActive && m_a2aLive.id == id)
        {
            cancelTask("任务已被删除");
            for (size_t k = 0; k < m_tasks.size(); ++k)
                m_tasks[k]->dropRunningHistory();
            ++removed;   // 列表里那条(running)已经删掉; 没有也照样取消
            ++canceled;
            continue;
        }
        if (id.size() < 2 || id[0] != 'h')
            continue;   // 不是历史条目
        int seq = 0;
        bool bad = false;
        for (size_t p = 1; p < id.size(); ++p)
        {
            if (id[p] < '0' || id[p] > '9') { bad = true; break; }
            seq = seq * 10 + (id[p] - '0');
        }
        if (bad)
            continue;
        bool hit = false;
        for (size_t k = 0; k < m_tasks.size() && !hit; ++k)
            hit = m_tasks[k]->removeHistory(seq);
        if (hit)
            ++removed;
        else
        {
            Json::Value s(Json::objectValue);
            s["id"] = id;
            s["reason"] = "没有这条记录(或在跑)";
            skipped.append(s);
        }
    }

    Json::Value j(Json::objectValue);
    j["ok"] = removed > 0;
    j["removed"] = removed;
    j["canceled"] = canceled;
    j["skipped"] = skipped;
    if (removed == 0)
        j["error"] = "没有这条记录";
    sendJson(client, removed > 0 ? 200 : 404, j);
}

void JZAgentCli::onApiTaskSubscribe(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req)) { sendJson(client, 401, Json::objectValue); return; }
    const std::string id = req.queryParam("id");
    bool live = false;
    Json::Value t = a2aFindTask(id, live);
    if (t.isNull())
    {
        Json::Value e; e["error"] = "没有这个 Task: " + id;
        sendJson(client, 404, e);
        return;
    }

    JZHttpRespone resp;
    resp.setStatusCode(200);
    resp.setContentType("text/event-stream");
    resp.setHeader("Cache-Control", "no-cache");
    resp.setHeader("Connection", "keep-alive");
    resp.setHeader("X-Accel-Buffering", "no");
    resp.setHeader("Transfer-Encoding", "chunked");
    client->write(resp.toHeadByteArray(0, false), false);

    std::string head = a2aSseChunk("task", a2aJsonLine(t));
    client->write(head.data(), (int)head.size(), false);

    if (!live)
    {
        client->write("0\r\n\r\n", 5, true);   // 终态任务: 快照发完就关流
        return;
    }
    m_a2aSubs[client] = id;
    connect(&client->sigDestroyed, this, [this, client](JZObject *) {
        m_a2aSubs.erase(client);
    });
}

void JZAgentCli::onApiAgent(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req)) { sendJson(client, 401, Json::objectValue); return; }

    Json::Value info(Json::objectValue);
    info["agentId"] = a2aAgentId();
    info["name"] = m_config.agentName;
    info["version"] = m_config.version;
    info["rootDir"] = m_config.rootDir;
    info["modelName"] = m_config.modelName;
    info["online"] = true;
    info["busy"] = anyBusy();
    info["taskId"] = m_a2aLiveActive ? m_a2aLive.id : std::string();
    info["builtin"] = false;
    info["nodes"] = nodesJson();

    Json::Value card(Json::objectValue);
    card["name"] = a2aAgentId();
    card["version"] = m_config.version;
    card["description"] = "JZAgent CLI (本机进程)";
    Json::Value ifs(Json::arrayValue);
    Json::Value itf(Json::objectValue);
    itf["url"] = "/api/tasks";
    itf["protocolBinding"] = "A2A";
    ifs.append(itf);
    card["supportedInterfaces"] = ifs;
    card["capabilities"]["streaming"] = true;
    Json::Value skills(Json::arrayValue);
    card["skills"] = skills;
    Json::Value cm(Json::objectValue);
    cm["rootDir"] = m_config.rootDir;
    cm["modelName"] = m_config.modelName;
    cm["nodes"] = info["nodes"];
    cm["online"] = true;
    cm["busy"] = anyBusy();
    card["metadata"] = cm;

    Json::Value out(Json::objectValue);
    out["kind"] = "cli";
    out["info"] = info;
    out["card"] = card;
    out["tasks"] = a2aHistoryJson(20);
    sendJson(client, 200, out);
}

void JZAgentCli::onApiStatus(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req)) { sendJson(client, 401, Json::objectValue); return; }
    Json::Value self(Json::objectValue);
    self["agentId"] = a2aAgentId();
    self["name"] = m_config.agentName;
    self["version"] = m_config.version;
    self["rootDir"] = m_config.rootDir;
    self["modelName"] = m_config.modelName;
    self["online"] = true;
    self["busy"] = anyBusy();
    self["taskId"] = m_a2aLiveActive ? m_a2aLive.id : std::string();
    self["builtin"] = false;
    self["lastSeen"] = (Json::Int64)a2aNowMs();
    self["nodes"] = nodesJson();

    Json::Value arr(Json::arrayValue);
    arr.append(self);
    Json::Value j(Json::objectValue);
    Json::Value hub(Json::objectValue);
    hub["listening"] = true;
    hub["port"] = m_config.httpPort;
    j["hub"] = hub;
    j["known"] = arr;
    j["online"] = arr;
    j["uptimeMs"] = 0;
    sendJson(client, 200, j);
}

void JZAgentCli::onDashboard(JZHttpServerClient *client, const JZHttpRequest &req)
{
    std::string dir = m_config.httpDir;
    if (dir.empty())
        dir = JZPlatform::applicationDirPath() + "/webui";
    std::ifstream ifs(dir + "/agent.html", std::ios::binary);
    if (!ifs)
    {
        JZHttpRespone resp;
        resp.setStatusCode(404);
        resp.setContentType("text/plain; charset=utf-8");
        resp.setBody("页面不存在: " + dir + "/agent.html");
        client->response(resp, req);
        return;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    JZHttpRespone resp;
    resp.setStatusCode(200);
    resp.setContentType("text/html; charset=utf-8");
    // 页面每次校验(no-cache): HTML 里只有 app.js/app.css 带 ?v=, 页面自己被缓存住了
    // 就永远拿不到新的 ?v=(改完前端刷新无效, 还可能出现"新脚本配旧模板"的 null 报错)
    resp.setHeader("Cache-Control", "no-cache");
    resp.setBody(ss.str());
    client->response(resp, req);
}

void JZAgentCli::removeEventClient(JZHttpServerClient *client)
{
    for (auto it = m_eventClients.begin(); it != m_eventClients.end(); )
    {
        if (*it == client)
            it = m_eventClients.erase(it);
        else
            ++it;
    }
}

void JZAgentCli::pushEvent(const std::string &type, const Json::Value &data)
{
    // 工作实例: 自己没有订阅者、也不连控制面, 事件统一交给 master(带上队列下标)
    if (m_eventSink)
    {
        m_eventSink(0, type, data);
        return;
    }

    // 这里只做"本地广播"。上报控制面由 JZAgentCliTask::pushEvent 负责 —— 它知道自己的
    // taskId, 而这里只能拿 m_tasks[0] 猜。以前两边都发, server 收到每个流式分片都是双份,
    // 网页上就出现"在在 node node33"这种重复。
    Json::Value local = data;
    if (local.isObject())
        local["queue"] = 0;
    emitLocalEvent(type, local);
}

void JZAgentCli::emitLocalEvent(const std::string &type, const Json::Value &data)
{
    a2aEmit(type, data);        // 网页对话页(A2A 订阅者)也吃这份事件
    if (m_eventClients.empty())
        return;

    Json::Value j;
    j["type"] = type;
    j["data"] = data;
    std::string payload = Json::FastWriter().write(j);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();
    std::string chunk = makeSseChunk(payload);
    for (JZHttpServerClient *c : m_eventClients)
        c->write(chunk.data(), (int)chunk.size(), false);
}

bool JZAgentCli::checkToken(const JZHttpRequest &req) const
{
    if (m_config.httpToken.empty())
        return true;   // 未配置 token: 不鉴权

    std::string tk = req.queryParam("token");
    if (tk.empty())
    {
        std::string auth = req.header("Authorization");
        if (auth.size() > 7 && str_toLower(auth.substr(0, 7)) == "bearer ")
            tk = auth.substr(7);
    }
    return tk == m_config.httpToken;
}

void JZAgentCli::onTaskSubmit(JZHttpServerClient *client, const JZHttpRequest &req)
{
    // 1) token 校验(若配置了 httpToken)
    if (!checkToken(req))
    {
        sendJson(client, 401, Json::objectValue);
        return;
    }

    // 2) 解析 JSON body
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(std::string(req.body().data(), req.body().size()), root) || !root.isObject())
    {
        Json::Value e; e["error"] = "invalid JSON body";
        sendJson(client, 400, e);
        return;
    }

    // 3) 基本校验(空任务 / 图片不合法 / 忙) -> 立即回包
    string preImgError;
    std::vector<std::string> preImages = extractImages(root, preImgError);
    if (!preImgError.empty())
    {
        Json::Value e; e["error"] = preImgError;
        sendJson(client, 400, e);
        return;
    }
    if (extractUserText(root).empty() && preImages.empty())
    {
        Json::Value e; e["error"] = "no prompt found (need 'prompt' / 'messages', or 'images')";
        sendJson(client, 400, e);
        return;
    }
    // 4) 交给调度: 有队列空闲就直接跑, 否则排队; 真的排满了 submitTask 会回 queue_full。
    //    ⚠ 多队列下每个任务各有自己的回包连接, 不能再用单个 m_pendingClient:
    //      用一个 shared 标记跟踪"这条连接还在不在", 断开后不再往野指针写。
    auto alive = std::make_shared<bool>(true);
    JZHttpServerClient *c = client;
    connect(static_cast<JZSignal<JZObject*>*>(&client->sigDestroyed), this, [alive](JZObject *)
    {
        *alive = false;
    });
    submitTask(root, [this, c, alive](const Json::Value &j)
    {
        if (!*alive)
            return;   // 提交方已断开: 任务照跑, 只是没人收结果(与 SSE 时代一致)
        const bool full = j.isMember("queue_full") && j["queue_full"].asBool();
        sendJson(c, full ? 429 : 200, j);
    });
}

// 任务提交核心: 由 HTTP(onTaskSubmit) 调用, 结果经 respond 回调回包。
// 引擎本体: 一个队列内的"提交 → 验证 → 重试"循环。由 submitTask(调度)或 master 直接调用。

void JZAgentCli::onReset(JZHttpServerClient *client, const JZHttpRequest &req)
{
    if (!checkToken(req))
    {
        sendJson(client, 401, Json::objectValue);
        return;
    }

    Json::Value root;
    Json::Reader reader;
    std::string scope = "task";
    if (reader.parse(std::string(req.body().data(), req.body().size()), root) && root.isObject())
        scope = root.get("scope", "task").asString();

    bool cancelRunning = (scope == "task");
    bool taskCancelled = false;

    // 有任务在跑: 先给调用方一个明确结果(status:"cancelled"), 再停 agent
    if (cancelRunning)
        taskCancelled = cancelTask("任务已被重置");
    else
        resetHistory();

    Json::Value d;
    d["scope"] = scope;
    d["cancelled"] = taskCancelled;
    pushEvent("reset", d);

    Json::Value r;
    r["status"] = "ok";
    r["scope"] = scope;
    r["cancelled"] = taskCancelled;
    sendJson(client, 200, r);
}

bool JZAgentCli::cancelTask(const std::string &reason)
{
    // 排队任务也算"在途": 派发/回收的窗口里可能 m_tasks 已空但 m_pending 还有任务,
    // 只判 m_tasks 会让 /reset 直接返回 false, 把排队任务留在队列里, 之后照样被派发。
    if (m_tasks.empty() && m_pending.empty())
        return false;
    if (m_cancelling)
        return false;

    m_cancelling = true;
    bool cancelled = false;

    // 排队任务也要走完生命周期: 每条都回一次 cancelled, 不能静默 clear 让 HTTP/server 永久等待。
    std::deque<QueuedTask> pending;
    pending.swap(m_pending);
    for (QueuedTask &t : pending)
    {
        Json::Value r;
        r["status"] = "cancelled";
        r["verify_rounds"] = 0;
        r["agent_reply"] = "";
        r["last_verify_output"] = "";
        r["error"] = reason;
        if (t.respond)
        {
            t.respond(r);
            cancelled = true;
        }
    }

    // 各队列只停自己的验证、终端、远端 exec 与会话。取消期间的回包回调不得重入 pumpQueue。
    for (JZAgentCliTask *w : m_tasks)
        if (w->cancelLocalTask(reason))
            cancelled = true;

    m_cancelling = false;
    return cancelled;
}

void JZAgentCli::onListScripts(JZHttpServerClient *client, const JZHttpRequest &req)
{
    (void)req;
    static const char *kExts[] = {".py", ".sh", ".bat", ".cmd", ".js", ".ps1", nullptr};
    std::vector<std::string> filters;
    for (int i = 0; kExts[i]; ++i)
        filters.push_back(kExts[i]);
    JZDir dir(m_config.rootDir);
    std::vector<std::string> files = dir.entryList(filters, JZDir::Files | JZDir::NoDotAndDotDot);

    Json::Value arr(Json::arrayValue);
    for (const auto &f : files)
        arr.append(f);
    Json::Value r;
    r["scripts"] = arr;
    sendJson(client, 200, r);
}


// 宿主聚合各任务对象的最近历史(任务自己只保留自己那一份)
Json::Value JZAgentCli::recentJson() const
{
    Json::Value arr(Json::arrayValue);
    for (JZAgentCliTask *t : m_tasks)
    {
        Json::Value one = t->recentJson();
        for (Json::ArrayIndex i = 0; i < one.size(); ++i)
            arr.append(one[i]);
    }
    return arr;
}

// 请求预校验(与任务内解析同一份实现): HTTP 入口在排队前先判空/判图片格式
string JZAgentCli::extractUserText(const Json::Value &params)
{
    const Json::Value &msgs = params["messages"];
    if (msgs.isArray())
    {
        string out;
        for (Json::ArrayIndex i = 0; i < msgs.size(); ++i)
        {
            const Json::Value &m = msgs[i];
            string role = m.get("role", "").asString();
            const Json::Value &c = m["content"];
            if (role != "user")
                continue;
            if (c.isString())
            {
                out += c.asString();
            }
            else if (c.isArray())
            {
                for (Json::ArrayIndex j = 0; j < c.size(); ++j)
                {
                    const Json::Value &p = c[j];
                    if (p.isObject() && p["type"].asString() == "text")
                        out += p["text"].asString();
                    else if (p.isString())
                        out += p.asString();
                }
            }
        }
        if (!out.empty())
            return out;
    }
    // 退化: 直接给 prompt / text 字段
    if (params.isMember("prompt") && params["prompt"].isString())
        return params["prompt"].asString();
    if (params.isMember("text") && params["text"].isString())
        return params["text"].asString();
    return string();
}

// 收集图片附件(多模态)。两种写法都收 —— 与 OpenAI 的 messages 一致, 网页/脚本可直接搬:
//   "images": ["data:image/png;base64,...", "http://host/a.png"]     (单张也允许直接给字符串)
//   "messages": [{role:"user", content:[{type:"text"},{type:"image_url",image_url:{url}}]}]
// 只认 data: URL 与 http(s) 链接(别的形态喂给模型只会拿到 400)。
// ⚠ 上限是硬约束(kMaxTaskImages / kMaxImageChars): 超了就报错, 不静默截断 —— 否则
//   提交方以为图发过去了、模型却看不到, 排查起来很费劲。

std::vector<std::string> JZAgentCli::extractImages(const Json::Value &params, string &error)
{
    std::vector<std::string> out;
    string bad;
    size_t total = 0;

    auto add = [&](const Json::Value &v) {
        if (!v.isString())
            return;
        string url = v.asString();
        if (url.empty())
            return;
        if ((int)url.size() > kMaxImageChars)
        {
            if (bad.empty()) bad = "单张图片过大(超过 " + to_string(kMaxImageChars) + " 字符)";
            return;
        }
        if (total + url.size() > (size_t)kMaxImagesTotalChars)
        {
            if (bad.empty()) bad = "图片合计过大(超过 " + to_string(kMaxImagesTotalChars) + " 字符)";
            return;
        }
        string head = str_toLower(url.substr(0, 5));
        if (head.compare(0, 5, "data:") != 0 && head.compare(0, 4, "http") != 0)
        {
            if (bad.empty()) bad = "images 只接受 data: URL 或 http(s) 链接";
            return;
        }
        if ((int)out.size() >= kMaxTaskImages)
        {
            if (bad.empty()) bad = "图片数量过多(超过 " + to_string(kMaxTaskImages) + " 张)";
            return;
        }
        total += url.size();
        out.push_back(url);
    };

    const Json::Value &images = params["images"];
    if (images.isArray())
    {
        for (Json::ArrayIndex i = 0; i < images.size(); ++i)
            add(images[i]);
    }
    else if (images.isString())
    {
        add(images);
    }

    // messages 写法: 只取 user 消息 content 数组里的 image_url 部分(与 extractUserText 同一套遍历)
    const Json::Value &msgs = params["messages"];
    if (msgs.isArray())
    {
        for (Json::ArrayIndex i = 0; i < msgs.size(); ++i)
        {
            const Json::Value &m = msgs[i];
            if (m.get("role", "").asString() != "user")
                continue;
            const Json::Value &c = m["content"];
            if (!c.isArray())
                continue;
            for (Json::ArrayIndex j = 0; j < c.size(); ++j)
            {
                const Json::Value &p = c[j];
                if (!p.isObject() || p["type"].asString() != "image_url")
                    continue;
                const Json::Value &iu = p["image_url"];
                if (iu.isObject())
                    add(iu.get("url", ""));
                else if (iu.isString())
                    add(iu);
            }
        }
    }

    // 一张都没收进来但确实有毛病 -> 报错; 收了部分(超数量)也报错, 让提交方知道被截断了
    if (!bad.empty())
        error = bad;
    return out;
}

// ---------------------------------------------------------------------------
// 出错自动重试
//   瞬时错误(连接失败/超时/5xx)不该让整个任务直接判失败 —— 把错误原因告诉模型,
//   让它从中断处继续做。分类来自 JZAgent::errorCode(): llmError() 里先置分类再发
//   sigError, 所以 onError 里读到的就是本次错误的分类。
// ---------------------------------------------------------------------------

// "HTTP 402 错误: ..." -> 1=瞬时(原样重发有意义) / 2=可修复(原样重发必然再失败)。
// 408/409/425/429 是超时/冲突/限流, 归瞬时; 其余 4xx 是"我们发出去的东西有问题"
// (400 参数或工具调用 JSON 不合法、413/422 等), 归可修复: 把原因丢回模型让它改一版。
// 没有状态码(连接失败/超时等)按瞬时处理。
static int httpErrorMode(const std::string &err)
{
    if (err.compare(0, 5, "HTTP ") != 0)
        return 1;

    int code = atoi(err.c_str() + 5);
    if (code <= 0)
        return 1;
    if (code >= 500 || code == 408 || code == 409 || code == 425 || code == 429)
        return 1;
    return (code < 400) ? 1 : 2;
}

void JZAgentCli::resetHistory()
{
    for (JZAgentCliTask *t : m_tasks)
        t->resetHistory();
}
