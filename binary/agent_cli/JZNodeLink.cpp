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
    JZTimer::singleShot(3000, this, [this, name]()
    {
        if (m_helloAcked)
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
        return;
    }

    if (type == NodePack_execResult)
    {
        JZNodePackExecResult *in = (JZNodePackExecResult *)pack.get();
        if (!m_pending || m_pending->runId != in->runId)
            return;   // 迟到的结果(已被超时收口)直接丢

        ExecResult r;
        r.ok = in->ok;
        r.exitCode = in->exitCode;
        r.timedOut = in->timedOut;
        r.durationMs = in->durationMs;
        r.cwd = in->cwd;
        r.output = in->output;
        r.error = in->error;
        finishPending(r, true);
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

bool JZNodeLink::exec(const std::string &command, const std::string &cwd, int timeoutMs,
                      const ExecCallback &cb, std::string &error)
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
    if (timeoutMs <= 0)
        timeoutMs = 120000;
    // 与本地 terminal 的前台上限保持一致(15 分钟), 别让远端跑成"永远不返回"
    if (timeoutMs > 15 * 60 * 1000)
        timeoutMs = 15 * 60 * 1000;

    auto pending = std::unique_ptr<Pending>(new Pending());
    pending->runId = "n" + std::to_string(++m_runIdGen) + "-" + m_config.name;
    pending->cb = cb;
    pending->timeoutMs = timeoutMs;

    const string runId = pending->runId;
    JZTimer *timer = new JZTimer(this);
    timer->setSingleShot(true);
    connect(&timer->sigTimeout, this, [this, runId]()
    {
        if (!m_pending || m_pending->runId != runId)
            return;
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
    timer->start(timeoutMs);

    m_pending = std::move(pending);

    JZNodePackExec exec;
    exec.runId = runId;
    exec.command = command;
    exec.cwd = cwd;
    exec.timeoutMs = timeoutMs;
    m_client->sendPacket(&exec);
    return true;
}

void JZNodeLink::cancelRunning()
{
    if (!m_pending)
        return;
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

    ExecCallback cb = m_pending->cb;
    m_pending.reset();

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
                                 const std::string &nodeList)
    : m_resolver(resolver)
    , m_defaultHost(defaultHost)
    , m_nodeList(nodeList)
{
    ToolDefine def;
    def.name = "remote_ssh";
    def.description =
        "在一台远端节点上执行一条 shell 命令(等价于 ssh 'cmd')。\n"
        "命令**整条交给远端 shell**: 管道(|)、重定向(> >>)、串联(&& || ;)、通配符(*)、变量($VAR) 都按 shell 语义工作。\n"
        "⚠ 远端没有常驻 shell: 每条命令都是新进程, cd / export 不跨命令保留 —— 要进目录执行就写 `cd <目录> && <命令>`。\n"
        "⚠ 文件是持久的(落盘的改动下一个命令能看到), 但 cwd 与环境变量不保留。\n"
        "可用节点:\n" + m_nodeList +
        "\nhost 省略 = 用默认节点。超时默认 120s(上限 15min), 超时会请求节点停止该命令。";

    def.parameters.push_back(makeSshParam("host", ParamType::String, "节点名(见上方列表); 省略 = 默认节点"));
    def.parameters.push_back(makeSshParam("command", ParamType::String, "要执行的完整 shell 命令"));
    def.parameters.push_back(makeSshParam("cwd", ParamType::String,
                                          "工作目录(相对路径按该节点工作区解析); 省略 = 节点工作区根"));
    def.parameters.push_back(makeSshParam("timeout_ms", ParamType::Number, "超时毫秒数, 默认 120000, 上限 900000"));
    def.required_parameters.push_back("command");
    setToolDefine(def);
}

static string formatResult(const JZNodeLink *link, const JZNodeLink::ExecResult &r)
{
    string text = "[node " + link->config().name + "] ";
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
    const string host = call.params.get("host", "").asString();
    const string command = call.params.get("command", "").asString();
    const string cwd = call.params.get("cwd", "").asString();
    int timeoutMs = call.params.get("timeout_ms", 120000).asInt();

    auto fail = [this, &call](const string &why)
    {
        ToolCallResult res;
        res.id = call.id;
        res.isOk = false;
        res.result = why;
        sigResult.emit(res);
    };

    if (command.empty())
    {
        fail("command 不能为空");
        return;
    }

    const string want = host.empty() ? m_defaultHost() : host;
    JZNodeLink *link = m_resolver(want);
    if (!link)
    {
        fail("没有这个节点: \"" + want + "\"。" + m_nodeList);
        return;
    }

    // ⚠ 回调是异步的(要等远端执行完), 所以 call.id 必须按值拷走,
    //   不能捕获 &call —— 那时 ToolCall 早就析构了。
    const string callId = call.id;
    string error;
    bool started = link->exec(command, cwd, timeoutMs,
        [this, callId, link](const JZNodeLink::ExecResult &r)
        {
            ToolCallResult res;
            res.id = callId;
            res.isOk = r.ok;
            res.result = formatResult(link, r);
            sigResult.emit(res);
        },
        error);
    if (!started)
        fail(error);
}
