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
    s << runId << command << cwd << timeoutMs;
}

void JZNodePackExec::loadFromStream(JZDataStream &s)
{
    s >> runId >> command >> cwd >> timeoutMs;
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
    s << runId << ok << exitCode << timedOut << durationMs << cwd << output << error;
}

void JZNodePackExecResult::loadFromStream(JZDataStream &s)
{
    s >> runId >> ok >> exitCode >> timedOut >> durationMs >> cwd >> output >> error;
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
    s << runId;
}

void JZNodePackExecCancel::loadFromStream(JZDataStream &s)
{
    s >> runId;
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
    });
}
