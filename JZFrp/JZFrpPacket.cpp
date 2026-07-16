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
    JZNetPackManager::instance()->registPack<JZFrpHand>(FrpPack_Hand);
    JZNetPackManager::instance()->registPack<JZFrpHandRes>(FrpPack_HandRes);
    JZNetPackManager::instance()->registPack<JZFrpTrans>(FrpPack_Trans);
}