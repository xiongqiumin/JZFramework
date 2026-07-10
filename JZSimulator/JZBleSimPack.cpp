#include "JZBleSimPack.h"
#include "JZNetPacket.h"

static bool s_bleSimRegistered = false;

JZBleSimPack::JZBleSimPack() {}
int JZBleSimPack::packetType() const { return NetPack_user + 1; }

void JZBleSimPack::saveToStream(JZDataStream &s) const
{
    s << map;
}

void JZBleSimPack::loadFromStream(JZDataStream &s)
{
    s >> map;
}

void JZBleSimPack::ensureRegistered()
{
    if (s_bleSimRegistered)
        return;
    JZNetPackManager::instance()->registPack<JZBleSimPack>(NetPack_user + 1);
    s_bleSimRegistered = true;
}
