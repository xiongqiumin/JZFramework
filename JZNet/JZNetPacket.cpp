#include <assert.h>
#include <mutex>
#include "JZNetPacket.h"
#include "JZNetFileTrans.h"

#ifdef HAS_JZFrp
#include "JZFrp/JZFrpPacket.h"
#endif

//JZNetPack
JZNetPack::JZNetPack()
{
}

JZNetPack::~JZNetPack()
{
}

void JZNetPack::saveToStream(JZDataStream &s) const
{
}

void JZNetPack::loadFromStream(JZDataStream &s)
{
}

//JZNetPackHand
JZNetPackHand::JZNetPackHand()
{
}

JZNetPackHand::~JZNetPackHand()
{
}

int JZNetPackHand::packetType() const
{
    return NetPack_hand;
}

void JZNetPackHand::saveToStream(JZDataStream& s) const
{
    s << nonce;
}

void JZNetPackHand::loadFromStream(JZDataStream& s)
{
    s >> nonce;
}

// JZNetPackHandRes
JZNetPackHandRes::JZNetPackHandRes()
{
}
JZNetPackHandRes::~JZNetPackHandRes()
{
}

int JZNetPackHandRes::packetType() const
{
    return NetPack_handRes;
}

void JZNetPackHandRes::saveToStream(JZDataStream &s) const
{
}

void JZNetPackHandRes::loadFromStream(JZDataStream& s)
{
}

// JZNetPackEcho
JZNetPackEcho::JZNetPackEcho()
{
}

JZNetPackEcho::~JZNetPackEcho()
{
}

int JZNetPackEcho::packetType() const
{
    return NetPack_echo;
}

void JZNetPackEcho::saveToStream(JZDataStream &s) const
{
    s << text;
}

void JZNetPackEcho::loadFromStream(JZDataStream &s)
{
    s >> text;
}

//JZNetPackManager
int JZNetPackManager::s_maxPackSize = MG_MAX_PACK_SIZE;

JZNetPackManager *JZNetPackManager::instance()
{
    static JZNetPackManager inst;
    return &inst;
}

JZNetPackManager::JZNetPackManager()
{
}

JZNetPackPtr JZNetPackManager::createPack(int type)
{    
    auto it = m_packFactory.find(type);
    if(it == m_packFactory.end())
        return JZNetPackPtr();

    return JZNetPackPtr(m_packFactory[type]());
}

void JZNetPackInit()
{
    // ⚠ 幂等 + 线程安全保护 (入口处, 只注册一次):
    //   · registPack 对"同一个 type 注册两次"有**断言**(重复注册 = 真错, 断言保留不动),
    //     而这个入口天生会被重复调用: JZApplication 构造 -> JZDriver::init() -> 这里,
    //     而 binary/frpc、agent_cli/server 等还会在 main 里**再显式调一次**;
    //     本函数内部又调 JZFrpPacketInit() ⇒ 少了这道保护, Debug 下直接断言崩
    //     (Release 下断言被去掉, 变成静默覆盖)。
    //   · call_once 同时保证多线程下只注册一次, 且后来的线程会等第一次注册**完成**
    //     (用 atomic 标志只能保证"不重复", 挡不住"抢在注册完成前就去发包")。
    static std::once_flag once;
    std::call_once(once, [] {
        JZNetPackManager::instance()->registPack<JZNetPackHand>(NetPack_hand);
        JZNetPackManager::instance()->registPack<JZNetPackHandRes>(NetPack_handRes);
        JZNetPackManager::instance()->registPack<JZNetPackEcho>(NetPack_echo);

        JZNetFileTrans::registPack();

    #ifdef HAS_JZFrp
        JZFrpPacketInit();
    #endif
    });
}