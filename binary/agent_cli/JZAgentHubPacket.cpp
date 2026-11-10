#include <mutex>

#include "JZAgentHubPacket.h"

// ---------------------------------------------------------------------------
// 与 JZAgentNodePacket 同一套规则: 只做字段序列化, 不含业务逻辑。
// 字段顺序一旦发布就不要改(两端版本不同会错位)。
// ---------------------------------------------------------------------------

// ---- hello ----
int JZHubPackHello::packetType() const
{
    return HubPack_hello;
}

void JZHubPackHello::saveToStream(JZDataStream &s) const
{
    s << agentId << name << version << rootDir << modelName << nodes;
}

void JZHubPackHello::loadFromStream(JZDataStream &s)
{
    s >> agentId >> name >> version >> rootDir >> modelName >> nodes;
}

// ---- helloAck ----
JZHubPackHelloAck::JZHubPackHelloAck()
{
    ok = false;
    acceptTasks = true;
    serverTimeMs = 0;
}

int JZHubPackHelloAck::packetType() const
{
    return HubPack_helloAck;
}

void JZHubPackHelloAck::saveToStream(JZDataStream &s) const
{
    s << ok << error << acceptTasks << serverTimeMs;
}

void JZHubPackHelloAck::loadFromStream(JZDataStream &s)
{
    s >> ok >> error >> acceptTasks >> serverTimeMs;
}

// ---- heartbeat ----
JZHubPackHeartbeat::JZHubPackHeartbeat()
{
    busy = false;
    round = 0;
}

int JZHubPackHeartbeat::packetType() const
{
    return HubPack_heartbeat;
}

void JZHubPackHeartbeat::saveToStream(JZDataStream &s) const
{
    s << busy << round << taskId << nodes;
}

void JZHubPackHeartbeat::loadFromStream(JZDataStream &s)
{
    s >> busy >> round >> taskId >> nodes;
}

// ---- taskSubmit ----
int JZHubPackTaskSubmit::packetType() const
{
    return HubPack_taskSubmit;
}

void JZHubPackTaskSubmit::saveToStream(JZDataStream &s) const
{
    s << taskId << body;
}

void JZHubPackTaskSubmit::loadFromStream(JZDataStream &s)
{
    s >> taskId >> body;
}

// ---- taskCancel ----
int JZHubPackTaskCancel::packetType() const
{
    return HubPack_taskCancel;
}

void JZHubPackTaskCancel::saveToStream(JZDataStream &s) const
{
    s << taskId;
}

void JZHubPackTaskCancel::loadFromStream(JZDataStream &s)
{
    s >> taskId;
}

// ---- reset ----
int JZHubPackReset::packetType() const
{
    return HubPack_reset;
}

void JZHubPackReset::saveToStream(JZDataStream &s) const
{
    s << scope;
}

void JZHubPackReset::loadFromStream(JZDataStream &s)
{
    s >> scope;
}

// ---- taskEvent ----
int JZHubPackTaskEvent::packetType() const
{
    return HubPack_taskEvent;
}

void JZHubPackTaskEvent::saveToStream(JZDataStream &s) const
{
    s << taskId << type << data;
}

void JZHubPackTaskEvent::loadFromStream(JZDataStream &s)
{
    s >> taskId >> type >> data;
}

// ---- taskEnd ----
int JZHubPackTaskEnd::packetType() const
{
    return HubPack_taskEnd;
}

void JZHubPackTaskEnd::saveToStream(JZDataStream &s) const
{
    s << taskId << result;
}

void JZHubPackTaskEnd::loadFromStream(JZDataStream &s)
{
    s >> taskId >> result;
}

void JZAgentHubPacketInit()
{
    // 幂等 + 线程安全(JZNetPackManager::registPack 重复注册会断言)
    static std::once_flag once;
    std::call_once(once, [] {
        JZNetPackManager *m = JZNetPackManager::instance();
        m->registPack<JZHubPackHello>(HubPack_hello);
        m->registPack<JZHubPackHelloAck>(HubPack_helloAck);
        m->registPack<JZHubPackHeartbeat>(HubPack_heartbeat);
        m->registPack<JZHubPackTaskSubmit>(HubPack_taskSubmit);
        m->registPack<JZHubPackTaskCancel>(HubPack_taskCancel);
        m->registPack<JZHubPackReset>(HubPack_reset);
        m->registPack<JZHubPackTaskEvent>(HubPack_taskEvent);
        m->registPack<JZHubPackTaskEnd>(HubPack_taskEnd);
    });
}
