#include <mutex>
#include "JZFrpPacket.h"
#include "JZNetPacketProcessor.h"

//JZFrpHand
int JZFrpHand::packetType() const
{
    return FrpPack_Hand;
}

void JZFrpHand::saveToStream(JZDataStream &s) const
{
    s << serverPortList;
}

void JZFrpHand::loadFromStream(JZDataStream &s)
{
    s >> serverPortList;
}

//JZFrpHandRes
JZFrpHandRes::JZFrpHandRes()
{
    code = 0;
}

int JZFrpHandRes::packetType() const
{
    return FrpPack_HandRes;
}

void JZFrpHandRes::saveToStream(JZDataStream& s) const
{
    s << code << message;
}

void JZFrpHandRes::loadFromStream(JZDataStream &s)
{
    s >> code >> message;
}

//JZFrpTrans
JZFrpTrans::JZFrpTrans()
{
    type = FrpTrans_None;
    serverPort = 0;
    connectId = 0;
}

JZFrpTrans::~JZFrpTrans()
{
}

int JZFrpTrans::packetType() const
{
    return FrpPack_Trans;
}

void JZFrpTrans::saveToStream(JZDataStream &s) const
{
    s << type;
    s << serverPort;
    s << connectId;  //id 
    s << data;
}

void JZFrpTrans::loadFromStream(JZDataStream &s)
{
    s >> type;
    s >> serverPort;
    s >> connectId;  //id 
    s >> data;
}

void JZFrpPacketInit()
{
    // ⚠ 幂等 + 线程安全保护 (与 JZNetPackInit 同一个理由, 详见那边的说明):
    //   JZNetPackInit() 内部会调本函数, 而调用方常常又跟着显式再调一次
    //   (binary/frpc 的 main、agent_cli/server 的 startFrp 都是) ⇒ 没有这道保护,
    //   重复注册会撞 registPack 的断言。
    static std::once_flag once;
    std::call_once(once, [] {
        JZNetPackManager::instance()->registPack<JZFrpHand>(FrpPack_Hand);
        JZNetPackManager::instance()->registPack<JZFrpHandRes>(FrpPack_HandRes);
        JZNetPackManager::instance()->registPack<JZFrpTrans>(FrpPack_Trans);
    });
}