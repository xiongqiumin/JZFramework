#include <assert.h>
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
    JZNetPackManager::instance()->registPack<JZNetPackHand>(NetPack_hand);
    JZNetPackManager::instance()->registPack<JZNetPackHandRes>(NetPack_handRes);
    JZNetPackManager::instance()->registPack<JZNetPackEcho>(NetPack_echo);

    JZNetFileTrans::registPack();

#ifdef HAS_JZFrp
    JZFrpPacketInit();
#endif
}