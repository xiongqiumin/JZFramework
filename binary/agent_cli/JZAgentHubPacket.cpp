#include <mutex>

#include "JZAgentHubPacket.h"
#include "JZCompress.h"
#include "JZDebug.h"

namespace
{
// zlib 比 gzip 少了文件头/尾, 适合已经由 JZNet 加密的内部协议。
// 只有压缩后确实更小才启用, 避免给小 JSON 增加无用开销。
const size_t kHubZipMinBytes = 4 * 1024;

bool hubZipEncode(const std::string &plain, std::string &wire, uint8_t &encoding)
{
    wire = plain;
    encoding = 0;
    if (plain.size() < kHubZipMinBytes)
        return true;

    JZByteArray in(plain.data(), (int)plain.size());
    auto r = jzCompressBlock(in, JZCompressAlgorithm::Zlib);
    if (!r.isOk() || r.value().isEmpty() || r.value().size() >= (int)plain.size())
        return true;

    wire.assign(r.value().constData(), r.value().size());
    encoding = 1;
    return true;
}

static uint8_t readOptionalEncoding(JZDataStream &s)
{
    uint8_t enc = 0;
    s >> enc;
    if (s.isError())
    {
        // 老包没有尾字段: 读到越界(或读到下个包的头字节)都回退为未压缩。
        s.clearError();
        return 0;
    }
    return enc <= 1 ? enc : 0;
}

bool hubZipDecode(const std::string &wire, uint8_t encoding, std::string &plain)
{
    if (encoding == 0)
    {
        plain = wire;
        return true;
    }
    if (encoding != 1)
    {
        jzWarning() << "控制面未知压缩编码:" << (int)encoding;
        return false;
    }

    JZByteArray in(wire.data(), (int)wire.size());
    auto r = jzDecompressBlock(in, JZCompressAlgorithm::Zlib);
    if (!r.isOk())
    {
        jzWarning() << "控制面 zlib 解压失败, 字节数:" << wire.size();
        return false;
    }
    plain.assign(r.value().constData(), r.value().size());
    return true;
}
} // namespace

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
    s << agentId << name << version << rootDir << project << modelName << nodes;
    s << contextWindow << modelWindows;
}

void JZHubPackHello::loadFromStream(JZDataStream &s)
{
    s >> agentId >> name >> version >> rootDir >> project >> modelName >> nodes;
    s >> contextWindow >> modelWindows;
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
    s << taskId << body << encoding;
}

void JZHubPackTaskSubmit::loadFromStream(JZDataStream &s)
{
    s >> taskId >> body;
    if (s.isError())
        return;
    encoding = readOptionalEncoding(s);
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
    s << taskId << type << data << encoding;
}

void JZHubPackTaskEvent::loadFromStream(JZDataStream &s)
{
    s >> taskId >> type >> data;
    if (s.isError())
        return;
    encoding = readOptionalEncoding(s);
}

// ---- taskEnd ----
int JZHubPackTaskEnd::packetType() const
{
    return HubPack_taskEnd;
}

void JZHubPackTaskEnd::saveToStream(JZDataStream &s) const
{
    s << taskId << result << encoding;
}

void JZHubPackTaskEnd::loadFromStream(JZDataStream &s)
{
    s >> taskId >> result;
    if (s.isError())
        return;
    encoding = readOptionalEncoding(s);
}

// ---- fsReq(server -> cli: 在 cli 本机做一次文件操作) ----
int JZHubPackFsReq::packetType() const
{
    return HubPack_fsReq;
}

void JZHubPackFsReq::saveToStream(JZDataStream &s) const
{
    s << reqId << op << body;
}

void JZHubPackFsReq::loadFromStream(JZDataStream &s)
{
    s >> reqId >> op >> body;
}

// ---- fsRes ----
JZHubPackFsRes::JZHubPackFsRes()
{
    ok = false;
    size = 0;
    mtime = 0;
    truncated = false;
}

int JZHubPackFsRes::packetType() const
{
    return HubPack_fsRes;
}

void JZHubPackFsRes::saveToStream(JZDataStream &s) const
{
    s << reqId << ok << error << body << size << mtime << truncated << encoding;
}

void JZHubPackFsRes::loadFromStream(JZDataStream &s)
{
    s >> reqId >> ok >> error >> body >> size >> mtime >> truncated;
    if (s.isError())
        return;
    encoding = readOptionalEncoding(s);
}

bool hubPackZipEncode(const std::string &plain, std::string &wire, uint8_t &encoding)
{
    return hubZipEncode(plain, wire, encoding);
}

bool hubPackZipDecode(const std::string &wire, uint8_t encoding, std::string &plain)
{
    return hubZipDecode(wire, encoding, plain);
}

bool hubPackSendable(const JZNetPack *pack)
{
    if (!pack)
        return false;

    // 与 JZNetPacketProcessor::formatPacket 的明文部分完全一致(加密后长度 = 明文长度 + 20,
    // 接收侧按明文长度判上限), 这样预检通过的包一定不会在 formatPacket 里触发超限。
    JZByteArray buffer;
    JZDataStream s(&buffer, JZ::WriteOnly);
    s.setMaxSize(JZNetPackManager::maxPackSize());
    s << (uint8_t)PACK_HEAD;
    s << (int32_t)0;
    s << (int32_t)pack->packetType();
    pack->saveToStream(s);
    return !s.isError() && (int)buffer.size() <= JZNetPackManager::maxPackSize();
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
        m->registPack<JZHubPackFsReq>(HubPack_fsReq);
        m->registPack<JZHubPackFsRes>(HubPack_fsRes);
    });
}
