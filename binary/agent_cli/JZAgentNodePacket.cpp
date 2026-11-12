#include <mutex>

#include "JZAgentNodePacket.h"

// ---------------------------------------------------------------------------
// 各包的收发。规则与 JZFrpPacket 一致: 只负责字段的序列化, 不含任何业务逻辑。
// ⚠ 字段顺序一旦发布就不能改(两端版本不同会错位), 要加字段只能追加到末尾。
// ---------------------------------------------------------------------------

// ---- hello ----
int JZNodePackHello::packetType() const
{
    return NodePack_hello;
}

void JZNodePackHello::saveToStream(JZDataStream &s) const
{
    s << clientId << cliVersion;
}

void JZNodePackHello::loadFromStream(JZDataStream &s)
{
    s >> clientId >> cliVersion;
}

// ---- helloAck ----
JZNodePackHelloAck::JZNodePackHelloAck()
{
    ok = false;
}

int JZNodePackHelloAck::packetType() const
{
    return NodePack_helloAck;
}

void JZNodePackHelloAck::saveToStream(JZDataStream &s) const
{
    s << ok << error << nodeName << version << rootDir << shell;
}

void JZNodePackHelloAck::loadFromStream(JZDataStream &s)
{
    s >> ok >> error >> nodeName >> version >> rootDir >> shell;
}

// ---- exec ----
JZNodePackExec::JZNodePackExec()
{
    timeoutMs = 120000;
}

int JZNodePackExec::packetType() const
{
    return NodePack_exec;
}

void JZNodePackExec::saveToStream(JZDataStream &s) const
{
    s << runId << command << cwd << timeoutMs << background << session;
}

void JZNodePackExec::loadFromStream(JZDataStream &s)
{
    s >> runId >> command >> cwd >> timeoutMs >> background >> session;
}

// ---- execResult ----
JZNodePackExecResult::JZNodePackExecResult()
{
    ok = false;
    exitCode = -1;
    timedOut = false;
    durationMs = 0;
}

int JZNodePackExecResult::packetType() const
{
    return NodePack_execResult;
}

void JZNodePackExecResult::saveToStream(JZDataStream &s) const
{
    s << runId << ok << exitCode << timedOut << durationMs << cwd << output << error << started;
}

void JZNodePackExecResult::loadFromStream(JZDataStream &s)
{
    s >> runId >> ok >> exitCode >> timedOut >> durationMs >> cwd >> output >> error >> started;
}

// ---- execCancel ----
JZNodePackExecCancel::JZNodePackExecCancel()
{
}

int JZNodePackExecCancel::packetType() const
{
    return NodePack_execCancel;
}

void JZNodePackExecCancel::saveToStream(JZDataStream &s) const
{
    s << runId << session;
}

void JZNodePackExecCancel::loadFromStream(JZDataStream &s)
{
    s >> runId >> session;
}

// ---- termQuery ----
JZNodePackTermQuery::JZNodePackTermQuery()
{
}

int JZNodePackTermQuery::packetType() const
{
    return NodePack_termQuery;
}

void JZNodePackTermQuery::saveToStream(JZDataStream &s) const
{
    s << runId << tail << grep << incremental;
}

void JZNodePackTermQuery::loadFromStream(JZDataStream &s)
{
    s >> runId >> tail >> grep >> incremental;
}

// ---- termQueryResult ----
JZNodePackTermQueryResult::JZNodePackTermQueryResult()
{
}

int JZNodePackTermQueryResult::packetType() const
{
    return NodePack_termQueryResult;
}

void JZNodePackTermQueryResult::saveToStream(JZDataStream &s) const
{
    s << runId << found << running << ok << background << timedOut << exitCode
      << durationMs << totalLines << firstLine << readPos << text << error;
}

void JZNodePackTermQueryResult::loadFromStream(JZDataStream &s)
{
    s >> runId >> found >> running >> ok >> background >> timedOut >> exitCode
      >> durationMs >> totalLines >> firstLine >> readPos >> text >> error;
}

// ---- ping / pong (保活) ----
int JZNodePackPing::packetType() const
{
    return NodePack_ping;
}

void JZNodePackPing::saveToStream(JZDataStream &s) const
{
    s << clientId;
}

void JZNodePackPing::loadFromStream(JZDataStream &s)
{
    s >> clientId;
}

JZNodePackPong::JZNodePackPong()
{
}

int JZNodePackPong::packetType() const
{
    return NodePack_pong;
}

void JZNodePackPong::saveToStream(JZDataStream &s) const
{
    s << atMs;
}

void JZNodePackPong::loadFromStream(JZDataStream &s)
{
    s >> atMs;
}

// ---- sess (会话管理) ----
JZNodePackSess::JZNodePackSess()
{
}

int JZNodePackSess::packetType() const
{
    return NodePack_sess;
}

void JZNodePackSess::saveToStream(JZDataStream &s) const
{
    s << reqId << op << session << cwd;
}

void JZNodePackSess::loadFromStream(JZDataStream &s)
{
    s >> reqId >> op >> session >> cwd;
}

JZNodePackSessResult::JZNodePackSessResult()
{
}

int JZNodePackSessResult::packetType() const
{
    return NodePack_sessResult;
}

void JZNodePackSessResult::saveToStream(JZDataStream &s) const
{
    s << reqId << ok << error << session << text;
}

void JZNodePackSessResult::loadFromStream(JZDataStream &s)
{
    s >> reqId >> ok >> error >> session >> text;
}

void JZAgentNodePacketInit()
{
    // 幂等 + 线程安全: 与 JZFrpPacketInit 同一个理由 —— 调用方可能重复初始化,
    // 而 registPack 里有"重复注册直接断言"的保护。
    static std::once_flag once;
    std::call_once(once, [] {
        JZNetPackManager *m = JZNetPackManager::instance();
        m->registPack<JZNodePackHello>(NodePack_hello);
        m->registPack<JZNodePackHelloAck>(NodePack_helloAck);
        m->registPack<JZNodePackExec>(NodePack_exec);
        m->registPack<JZNodePackExecResult>(NodePack_execResult);
        m->registPack<JZNodePackExecCancel>(NodePack_execCancel);
        m->registPack<JZNodePackTermQuery>(NodePack_termQuery);
        m->registPack<JZNodePackTermQueryResult>(NodePack_termQueryResult);
        m->registPack<JZNodePackPing>(NodePack_ping);
        m->registPack<JZNodePackPong>(NodePack_pong);
        m->registPack<JZNodePackSess>(NodePack_sess);
        m->registPack<JZNodePackSessResult>(NodePack_sessResult);
    });
}
