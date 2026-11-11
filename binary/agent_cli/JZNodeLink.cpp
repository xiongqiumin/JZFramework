#include "JZNodeLink.h"

#include <algorithm>

#include "JZAgentNodePacket.h"
#include "JZDateTime.h"
#include "JZDebug.h"
#include "JZNet/JZNetClient.h"
#include "JZString.h"
#include "JZTimer.h"

using namespace std;

// ===========================================================================
// JZNodeLink
// ===========================================================================

JZNodeLink::JZNodeLink(const Config &config, const std::string &clientId, JZObject *parent)
    : JZObject(parent)
    , sigStateChanged(this)
    , m_config(config)
    , m_clientId(clientId)
{
}

JZNodeLink::~JZNodeLink()
{
    if (m_client)
    {
        m_client->disconnectFromHost();
        m_client->deleteLater();
        m_client = nullptr;
    }
}

void JZNodeLink::connectToNode()
{
    if (m_client)
        return;

    m_client = new JZNetClient(this);
    if (!m_config.token.empty())
        m_client->setToken(m_config.token);

    connect(&m_client->sigConnected, this, &JZNodeLink::onConnected);
    connect(&m_client->sigDisconnected, this, &JZNodeLink::onDisconnected);
    connect(&m_client->sigPacketReceive, this, [this](JZNetPackPtr pack) { onPacket(pack); });

    jzInfo().noquote().nospace() << "连接节点 " << m_config.name << " -> " << m_config.host
                                 << ":" << m_config.port;
    m_client->connectToHost(m_config.host, m_config.port);
}

void JZNodeLink::onConnected()
{
    // 连接建立 ≠ 握手完成: 先发 hello, 收到 helloAck 才算"可用"。
    // 为什么必须等 ack: JZNet 的 token 只是派生加密密钥(不比对内容), token 写错的表现
    // 是"包解不开", 不报认证失败 —— 靠这里的超时给出明确原因。
    JZNodePackHello hello;
    hello.clientId = m_clientId;
    hello.cliVersion = "1.0.0";
    if (m_client)
        m_client->sendPacket(&hello);

    const string name = m_config.name;
    const int gen = ++m_connGen;
    JZTimer::singleShot(3000, this, [this, name, gen]()
    {
        // 旧连接的超时不能作用在新连接上: 只认"最后一次 onConnected"的那一代
        if (gen != m_connGen || m_helloAcked)
            return;
        m_lastError = "节点 " + name + " 没有回应 hello(3s): 多半是 token 不匹配或端口不对";
        jzWarning().noquote().nospace() << m_lastError;
        if (m_client)
        {
            m_client->disconnectFromHost();
        }
    });
}

void JZNodeLink::onDisconnected()
{
    const bool wasOnline = m_online;
    m_online = false;
    m_helloAcked = false;

    if (m_pending)
    {
        ExecResult r;
        r.error = "与节点 " + m_config.name + " 的连接已断开, 命令结果未知";
        finishPending(r, true);
    }

    if (m_query)
    {
        QueryResult q;
        q.error = "与节点 " + m_config.name + " 的连接已断开, 查不到运行状态";
        finishQuery(q, true);
    }

    if (!m_bgRuns.empty())
    {
        // 后台命令可能还在远端跑, 但已经没法查/停了: 结果未知, 不假装它结束
        jzWarning().noquote().nospace() << "节点 " << m_config.name << " 断开时还有 "
                                        << m_bgRuns.size() << " 条后台命令, 结果未知";
        m_bgRuns.clear();
    }

    if (wasOnline)
        jzWarning().noquote().nospace() << "节点断开: " << m_config.name;
    sigStateChanged.emit();
    scheduleReconnect();
}

void JZNodeLink::onPacket(JZNetPackPtr pack)
{
    if (!pack)
        return;

    const int type = pack->packetType();
    if (type == NodePack_helloAck)
    {
        JZNodePackHelloAck *ack = (JZNodePackHelloAck *)pack.get();
        m_helloAcked = true;
        m_nodeName = ack->nodeName;
        m_rootDir = ack->rootDir;
        m_shell = ack->shell;
        m_online = ack->ok;
        m_lastError = ack->ok ? string() : ack->error;
        m_retryMs = 1000;
        if (m_online)
            jzInfo().noquote().nospace() << "节点就绪: " << m_config.name << " (name=" << m_nodeName
                                         << " root=" << m_rootDir << ")";
        else
            jzWarning().noquote().nospace() << "节点拒绝服务: " << m_config.name << "  " << ack->error;
        sigStateChanged.emit();

        // ack=false 但连接还在: 不主动断开的话, onDisconnected 永远不会来, 退避重连也就
        // 不会发生 —— 节点会一直挂着"离线"直到进程重启或网络真断。断开让它走正常重连。
        if (!ack->ok && m_client)
            m_client->disconnectFromHost();
        return;
    }

    if (type == NodePack_execResult)
    {
        JZNodePackExecResult *in = (JZNodePackExecResult *)pack.get();
        if (!m_pending || m_pending->runId != in->runId)
        {
            // 后台命令的"结束"通知(启动时已回过 started, 现在只是收尾): 摘掉"在跑"标记,
            // 结果本身以 termQuery 为准, 这里不重复报。
            if (m_bgRuns.erase(in->runId) > 0)
                jzInfo().noquote().nospace() << "后台命令结束: " << in->runId
                                             << (in->ok ? " (ok)" : (" (失败: " + in->error + ")"));
            return;   // 其它情况是迟到的结果(已被超时收口)直接丢
        }

        ExecResult r;
        r.ok = in->ok;
        r.started = in->started;
        r.exitCode = in->exitCode;
        r.timedOut = in->timedOut;
        r.durationMs = in->durationMs;
        r.cwd = in->cwd;
        r.output = in->output;
        r.error = in->error;

        if (in->started)
            m_bgRuns[in->runId] = m_pending->owner;   // 之后靠 query/cancel 收口

        finishPending(r, true);
        return;
    }

    if (type == NodePack_termQueryResult)
    {
        JZNodePackTermQueryResult *in = (JZNodePackTermQueryResult *)pack.get();
        if (!m_query)
            return;
        // 省略 run_id 时请求里是空串, 节点会回它实际用的那个 id ⇒ 空就按"本次查询"匹配
        if (!m_query->runId.empty() && m_query->runId != in->runId)
            return;

        QueryResult q;
        q.runId = in->runId;
        q.found = in->found;
        q.running = in->running;
        q.ok = in->ok;
        q.background = in->background;
        q.timedOut = in->timedOut;
        q.exitCode = in->exitCode;
        q.durationMs = in->durationMs;
        q.totalLines = in->totalLines;
        q.firstLine = in->firstLine;
        q.readPos = in->readPos;
        q.text = in->text;
        q.error = in->error;

        // 已经结束(或节点上找不到记录)就不再算"在跑", 否则 busy() 永远为真
        if (!in->found || !in->running)
            m_bgRuns.erase(in->runId);

        finishQuery(q, true);
        return;
    }
}

void JZNodeLink::scheduleReconnect()
{
    if (!m_config.name.empty() && m_config.host.empty())
        return;   // 没配地址, 不重连

    const int delayMs = m_retryMs;
    m_retryMs = min(m_retryMs * 2, 30000);

    jzInfo().noquote().nospace() << "节点 " << m_config.name << " " << delayMs << "ms 后重连";
    JZTimer::singleShot(delayMs, this, [this]()
    {
        if (m_client)
        {
            m_client->deleteLater();
            m_client = nullptr;
        }
        connectToNode();
    });
}

bool JZNodeLink::exec(const std::string &command, const std::string &cwd, int timeoutMs, bool background,
                      const void *owner, const ExecCallback &cb, std::string &error)
{
    if (command.empty())
    {
        error = "command 为空";
        return false;
    }
    if (!m_online || !m_client)
    {
        error = "节点 " + m_config.name + " 离线" + (m_lastError.empty() ? string("(还没连上)") : "(" + m_lastError + ")");
        return false;
    }
    if (m_pending)
    {
        error = "节点 " + m_config.name + " 正忙(同时只跑一条命令)";
        return false;
    }
    // 后台命令仍占着节点那唯一的前台终端(节点侧串行): 本地不拦的话,
    // 请求会白跑一趟网络再被节点打回"节点忙"。
    if (!m_bgRuns.empty())
    {
        error = "节点 " + m_config.name + " 正忙(上一条命令还在跑, 等它结束再来)";
        return false;
    }
    if (timeoutMs <= 0)
        timeoutMs = 120000;
    // 与本地 terminal 的前台上限保持一致(15 分钟), 别让远端跑成"永远不返回"
    if (timeoutMs > 15 * 60 * 1000)
        timeoutMs = 15 * 60 * 1000;

    auto pending = std::unique_ptr<Pending>(new Pending());
    // runId 带节点前缀: 模型可能同时持有本地 run_id 与远端 run_id, 前缀让它能路由回来
    pending->runId = m_config.name + ":r" + std::to_string(++m_runIdGen);
    pending->cb = cb;
    pending->timeoutMs = timeoutMs;
    pending->owner = owner;

    const string runId = pending->runId;
    // 后台命令本身没有超时(与 terminal_run background 一致), 但"节点确认已启动"要有超时
    const int waitMs = background ? 15000 : timeoutMs;

    JZTimer *timer = new JZTimer(this);
    timer->setSingleShot(true);
    connect(&timer->sigTimeout, this, [this, runId, background]()
    {
        if (!m_pending || m_pending->runId != runId)
            return;
        if (background)
        {
            // 还没收到 "已启动": 命令可能跑了, 也可能没跑 —— 不重放, 让调用方按"结果未知"处理
            ExecResult r;
            r.error = "节点没有确认后台命令已启动(15s), 结果未知";
            finishPending(r, true);
            return;
        }
        // 超时: 先请节点停掉, 再本地收口(不等它的回包 —— 它可能已经卡死)
        if (m_client)
        {
            JZNodePackExecCancel cancel;
            cancel.runId = runId;
            m_client->sendPacket(&cancel);
        }
        ExecResult r;
        r.timedOut = true;
        r.error = "执行超时(" + std::to_string(m_pending->timeoutMs) + "ms), 已请求节点停止";
        finishPending(r, true);
    });
    pending->timer = timer;
    timer->start(waitMs);

    m_pending = std::move(pending);

    JZNodePackExec exec;
    exec.runId = runId;
    exec.command = command;
    exec.cwd = cwd;
    exec.timeoutMs = timeoutMs;
    exec.background = background;
    m_client->sendPacket(&exec);
    return true;
}

bool JZNodeLink::query(const std::string &runId, int tail, const std::string &grep, bool incremental,
                       const QueryCallback &cb, std::string &error)
{
    if (!m_online || !m_client)
    {
        error = "节点 " + m_config.name + " 离线" + (m_lastError.empty() ? string("(还没连上)") : "(" + m_lastError + ")");
        return false;
    }
    if (m_query)
    {
        error = "上一次查询还没回";
        return false;
    }
    // runId 可以为空 = 该节点最近一次运行(与本地 terminal_query 同口径, 由节点侧解析)

    m_query.reset(new QueryPending());
    m_query->runId = runId;
    m_query->cb = cb;

    const string key = runId;
    JZTimer *timer = new JZTimer(this);
    timer->setSingleShot(true);
    connect(&timer->sigTimeout, this, [this, key]()
    {
        if (!m_query || m_query->runId != key)
            return;
        QueryResult q;
        q.error = "查询超时(节点 10s 没回)";
        finishQuery(q, true);
    });
    timer->start(10000);
    m_query->timer = timer;

    JZNodePackTermQuery q;
    q.runId = runId;
    q.tail = tail;
    q.grep = grep;
    q.incremental = incremental;
    m_client->sendPacket(&q);
    return true;
}

bool JZNodeLink::cancelRun(const std::string &runId, std::string &error)
{
    if (!m_online || !m_client)
    {
        error = "节点 " + m_config.name + " 离线";
        return false;
    }
    if (runId.empty())
    {
        error = "run_id 为空";
        return false;
    }

    JZNodePackExecCancel cancel;
    cancel.runId = runId;
    m_client->sendPacket(&cancel);

    if (m_pending && m_pending->runId == runId)
    {
        ExecResult r;
        r.error = "已请求节点停止";
        finishPending(r, true);
    }
    m_bgRuns.erase(runId);
    return true;
}

void JZNodeLink::cancelRunning(const void *owner)
{
    if (m_pending && (!owner || m_pending->owner == owner))
    {
        if (m_client)
        {
            JZNodePackExecCancel cancel;
            cancel.runId = m_pending->runId;
            m_client->sendPacket(&cancel);
        }
        ExecResult r;
        r.error = "任务被重置, 已请求节点停止";
        finishPending(r, true);
    }

    // 后台命令也要停: 否则任务结束了, 远端还在跑
    if (m_client)
    {
        for (auto it = m_bgRuns.begin(); it != m_bgRuns.end();)
        {
            if (owner && it->second != owner)
            {
                ++it;
                continue;
            }
            JZNodePackExecCancel cancel;
            cancel.runId = it->first;
            m_client->sendPacket(&cancel);
            it = m_bgRuns.erase(it);
        }
    }
}

void JZNodeLink::finishPending(const ExecResult &result, bool notify)
{
    if (!m_pending)
        return;

    if (m_pending->timer)
    {
        m_pending->timer->stop();
        m_pending->timer->deleteLater();
        m_pending->timer = nullptr;
    }

    // runId 统一在收口时补齐: 模型要拿它去 query/cancel, 不能只靠调用方自己记
    ExecResult r = result;
    if (r.runId.empty())
        r.runId = m_pending->runId;

    ExecCallback cb = m_pending->cb;
    m_pending.reset();

    if (notify && cb)
        cb(r);
}

void JZNodeLink::finishQuery(const QueryResult &result, bool notify)
{
    if (!m_query)
        return;

    if (m_query->timer)
    {
        m_query->timer->stop();
        m_query->timer->deleteLater();
        m_query->timer = nullptr;
    }

    QueryCallback cb = m_query->cb;
    m_query.reset();

    if (notify && cb)
        cb(result);
}

// ===========================================================================
// JZRemoteSshTool
// ===========================================================================

static ParamSchema makeSshParam(const string &name, ParamType type, const string &desc)
{
    ParamSchema p;
    p.name = name;
    p.description = desc;
    p.type = type;
    return p;
}

JZRemoteSshTool::JZRemoteSshTool(std::function<JZNodeLink *(const std::string &)> resolver,
                                 std::function<std::string()> defaultHost,
                                 const std::string &nodeList, const void *owner)
    : m_resolver(resolver)
    , m_defaultHost(defaultHost)
    , m_nodeList(nodeList)
    , m_owner(owner)
{
    ToolDefine def;
    def.name = "remote_ssh";
    def.description =
        "在一台远端节点上执行一条 shell 命令(语义与本地 terminal_run 一致, 只是走网络)。\n"
        "命令**整条交给远端 shell**: 管道(|)、重定向(> >>)、串联(&& || ;)、通配符(*)、变量($VAR) 都按 shell 语义工作。\n"
        "⚠ 远端是**有状态的 shell**: cd / export 会保留到下一条命令(文件当然也持久)。\n"
        "⚠ 节点是**单前台串行**: 上一条没结束, 下一条直接报「节点忙」。\n"
        "⚠ 没有后台/查询: 要跑常驻服务就在命令里写 `&` 并自己重定向"
        "(`./server > server.log 2>&1 &`), 之后用 `cat server.log` / `tail -50 server.log` 看。\n"
        "可用节点:\n" + m_nodeList +
        "\nhost 省略 = 用默认节点。超时默认 120s(上限 15min), 超时会请求节点停止该命令。";

    def.parameters.push_back(makeSshParam("host", ParamType::String, "节点名(见上方列表); 省略 = 默认节点"));
    def.parameters.push_back(makeSshParam("command", ParamType::String, "要执行的完整 shell 命令"));
    def.parameters.push_back(makeSshParam("cwd", ParamType::String,
                                          "工作目录(相对路径按该节点工作区解析); 省略 = 节点工作区根"));
    def.parameters.push_back(makeSshParam("timeout_ms", ParamType::Number, "超时毫秒数, 默认 120000, 上限 900000"));
    setToolDefine(def);
}

static string formatRun(const string &node, const JZNodeLink::ExecResult &r)
{
    string text = "[node " + node + "] ";
    if (r.ok)
        text += "exit=0";
    else if (r.timedOut)
        text += "超时";
    else
        text += "失败" + (r.exitCode >= 0 ? ("(exit=" + to_string(r.exitCode) + ")") : string());
    if (r.durationMs > 0)
        text += " " + to_string(r.durationMs) + "ms";
    if (!r.cwd.empty())
        text += "  cwd=" + r.cwd;
    if (!r.error.empty())
        text += "  " + r.error;
    text += "\n";
    text += r.output.empty() ? string("(无输出)") : r.output;
    return text;
}

void JZRemoteSshTool::call(const ToolCall &call)
{
    const string action = call.params.get("action", "run").asString();
    (void)action;   // 只剩 run 一种动作(参数保留给旧调用方, 值不再影响行为)
    const string host = call.params.get("host", "").asString();
    const string command = call.params.get("command", "").asString();
    const string cwd = call.params.get("cwd", "").asString();
    int timeoutMs = call.params.get("timeout_ms", 120000).asInt();

    // ⚠ 回调是异步的(要等远端回包), 所以 call.id 必须按值拷走,
    //   不能捕获 &call —— 那时 ToolCall 早就析构了。
    const string callId = call.id;
    auto fail = [this, callId](const string &why)
    {
        ToolCallResult res;
        res.id = callId;
        res.isOk = false;
        res.result = why;
        sigResult.emit(res);
    };
    auto done = [this, callId](const string &text, bool ok)
    {
        ToolCallResult res;
        res.id = callId;
        res.isOk = ok;
        res.result = text;
        sigResult.emit(res);
    };

    // ---- run ----
    if (command.empty())
    {
        fail("action=run 需要 command");
        return;
    }

    const string want = host.empty() ? m_defaultHost() : host;
    JZNodeLink *link = m_resolver(want);
    if (!link)
    {
        fail("没有这个节点: \"" + want + "\"。\n" + m_nodeList);
        return;
    }

    // 不再有 background 参数: 后台靠命令里的 `&`(与本地终端同一套语义)
    const bool background = false;
    const string nodeName = link->config().name;
    string error;
    bool started = link->exec(command, cwd, timeoutMs, background, m_owner,
        [this, done, nodeName](const JZNodeLink::ExecResult &r)
        {
            done(formatRun(nodeName, r), r.ok);
        },
        error);
    if (!started)
        fail(error);
}
