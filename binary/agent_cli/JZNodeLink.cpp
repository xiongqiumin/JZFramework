#include "JZNodeLink.h"

#include <algorithm>

#include "JZAgentNodePacket.h"
#include "JZCodeTools.h"   // jzTerminalRunNotes: 终端口径与本地 terminal_run 共用一份
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
    if (m_pingTimer)
        m_pingTimer->stop();

    if (m_sess)
        finishSession(false, std::string(), "与节点的连接已断开");

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
    if (type == NodePack_pong)
        return;   // 保活回应: 说明 node 还活着(断开仍由 socket 报, 这里不做额外判定)

    if (type == NodePack_sessResult)
    {
        JZNodePackSessResult *in = (JZNodePackSessResult *)pack.get();
        if (!m_sess || m_sess->reqId != in->reqId)
            return;   // 迟到的回包(已被超时收口)
        finishSession(in->ok, in->text, in->error);
        return;
    }

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
        {
            jzInfo().noquote().nospace() << "节点就绪: " << m_config.name << " (name=" << m_nodeName
                                         << " root=" << m_rootDir << ")";
            // 保活只在握手成功之后跑: socket 半开时只有定期的 ping 能让 node 发现本端已死,
            // 从而按 shell 的语义 kill 掉在跑的命令(不做断线续跑)。
            if (!m_pingTimer)
            {
                m_pingTimer = new JZTimer(this);
                connect(&m_pingTimer->sigTimeout, this, [this]() { sendPing(); });
            }
            m_pingTimer->start(5000);
            sendPing();   // 立刻发一发: node 从"握手完成"这一刻起就把这条连接纳入保活判定
        }
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

void JZNodeLink::sendPing()
{
    if (!m_client || !m_online)
        return;
    JZNodePackPing ping;
    ping.clientId = m_clientId;
    m_client->sendPacket(&ping);
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

std::string JZNodeLink::sessionFor(const void *owner)
{
    auto it = m_ownerSession.find(owner);
    if (it != m_ownerSession.end())
        return it->second;
    const std::string sid = "q" + std::to_string(m_sessionGen++);
    m_ownerSession[owner] = sid;
    return sid;
}

bool JZNodeLink::sessionOp(const std::string &op, const std::string &session, const std::string &cwd,
                           const SessionCallback &cb, std::string &error)
{
    if (!m_online || !m_client)
    {
        error = "节点 " + m_config.name + " 离线" + (m_lastError.empty() ? string("(还没连上)") : "(" + m_lastError + ")");
        return false;
    }
    if (m_sess)
    {
        error = "上一次会话操作还没回, 等一下再来";
        return false;
    }

    m_sess.reset(new SessPending());
    m_sess->reqId = m_config.name + ":s" + std::to_string(++m_sessReqGen);
    m_sess->cb = cb;

    const string reqId = m_sess->reqId;
    JZTimer *timer = new JZTimer(this);
    timer->setSingleShot(true);
    connect(&timer->sigTimeout, this, [this, reqId]()
    {
        if (!m_sess || m_sess->reqId != reqId)
            return;
        finishSession(false, std::string(), "节点 10s 没回会话操作(网络/版本不一致?)");
    });
    m_sess->timer = timer;
    timer->start(10000);

    JZNodePackSess req;
    req.reqId = reqId;
    req.op = op;
    req.session = session;
    req.cwd = cwd;
    m_client->sendPacket(&req);
    return true;
}

void JZNodeLink::finishSession(bool ok, const std::string &text, const std::string &error)
{
    if (!m_sess)
        return;
    SessPending done = *m_sess;   // 回调里可能又发一条, 先搬出来
    m_sess.reset();
    if (done.timer)
    {
        done.timer->stop();
        done.timer->deleteLater();
    }
    if (done.cb)
        done.cb(ok, text, error);
}

bool JZNodeLink::exec(const std::string &command, const std::string &cwd, int timeoutMs, bool background,
                      const void *owner, const ExecCallback &cb, std::string &error,
                      const std::string &session)
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
    // 模型显式指定了就按它, 否则用这个任务队列自己的终端
    pending->session = session.empty() ? sessionFor(owner) : session;

    const string runId = pending->runId;
    const string execSession = pending->session;
    // 后台命令本身没有超时(与 terminal_run background 一致), 但"节点确认已启动"要有超时
    const int waitMs = background ? 15000 : timeoutMs;

    JZTimer *timer = new JZTimer(this);
    timer->setSingleShot(true);
    connect(&timer->sigTimeout, this, [this, runId, execSession, background]()
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
            cancel.session = execSession;
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
    exec.session = execSession;
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
    if (m_pending && m_pending->runId == runId)
        cancel.session = m_pending->session;
    else
    {
        auto bi = m_bgRuns.find(runId);
        if (bi != m_bgRuns.end())
            cancel.session = sessionFor(bi->second);
    }
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
            cancel.session = m_pending->session;
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
            cancel.session = sessionFor(it->second);
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
                                 const std::string &nodeList, const void *owner)
    : m_resolver(resolver)
    , m_nodeList(nodeList)
    , m_owner(owner)
{
    ToolDefine def;
    def.name = "remote_ssh";
    // ⚠ 语法/前台/后台/不支持清单**不要在引用一份**: 与本地 terminal_run 共用 jzTerminalRunNotes(),
    //   否则两边迟早各说一套(以前这里写的"没有 background 参数"就和实际不符)。
    def.description =
        "在远端节点上执行命令 / 管理远端终端会话(语义与本地 terminal **完全一致**, 只是走网络)。\n"
        + jzTerminalRunNotes() +
        "  - session 省略 = 用当前任务自己的终端; open 时可自己起名(便于之后按名字复用);"
        " 每台节点的会话各自独立, 要并行就在同一节点上开多个 session。\n"
        "  - 这里的超时默认 120s(上限 15min), 超时会请求节点停止该命令。\n"
        "可用节点(host 必填, 就是节点名):\n" + m_nodeList +
        "\naction: run(默认) / open / close / list。";

    def.parameters.push_back(makeSshParam("host", ParamType::String, "节点名(必填, 见上方列表)"));
    def.parameters.push_back(makeSshParam("action", ParamType::String,
                                          "run(默认)=跑命令; open=开/复用会话; close=关会话(在跑的命令会被终止); list=列会话"));
    def.parameters.push_back(makeSshParam("command", ParamType::String, "action=run 时要执行的完整 shell 命令"));
    def.parameters.push_back(makeSshParam("session", ParamType::String,
                                          "会话名: run 时指定用哪台终端(省略 = 当前任务自己的); open 时可省略(节点自动起名); close 时必填"));
    def.parameters.push_back(makeSshParam("cwd", ParamType::String,
                                          "工作目录(相对路径按该节点工作区解析); 省略 = 节点工作区根 / 会话当前目录"));
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
    string action = call.params.get("action", "run").asString();
    if (action.empty())
        action = "run";
    const string host = call.params.get("host", "").asString();
    const string command = call.params.get("command", "").asString();
    const string cwd = call.params.get("cwd", "").asString();
    const string session = call.params.get("session", "").asString();
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

    if (host.empty())
    {
        fail("host 必填(就是节点名)。\n" + m_nodeList);
        return;
    }
    JZNodeLink *link = m_resolver(host);
    if (!link)
    {
        fail("没有这个节点: \"" + host + "\"。\n" + m_nodeList);
        return;
    }

    // ---- 会话管理(open / close / list) ----
    if (action == "open" || action == "close" || action == "list")
    {
        if (action == "close" && session.empty())
        {
            fail("action=close 需要 session(先 list 看看有哪些)");
            return;
        }
        const string nodeName = link->config().name;
        string opError;
        const bool started = link->sessionOp(action, session, cwd,
            [done, nodeName, action](bool ok, const string &text, const string &err)
            {
                string head = "[node " + nodeName + "] " + action;
                if (!ok)
                {
                    done(head + " 失败: " + err, false);
                    return;
                }
                done(head + "\n" + text, true);
            },
            opError);
        if (!started)
            fail(opError);
        return;
    }

    if (action != "run")
    {
        fail("未知的 action: " + action + "(支持 run / open / close / list)");
        return;
    }

    // ---- run ----
    if (command.empty())
    {
        fail("action=run 需要 command");
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
        error, session);
    if (!started)
        fail(error);
}
