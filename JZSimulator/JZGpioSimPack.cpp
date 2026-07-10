#include "JZGpioSimPack.h"
#include "JZNetPacket.h"

static bool s_gpioSimRegistered = false;

JZGpioSimPack::JZGpioSimPack() {}
int JZGpioSimPack::packetType() const { return NetPack_user + 2; }

void JZGpioSimPack::saveToStream(JZDataStream &s) const
{
    s << map;
}

void JZGpioSimPack::loadFromStream(JZDataStream &s)
{
    s >> map;
}

void JZGpioSimPack::ensureRegistered()
{
    if (s_gpioSimRegistered)
        return;
    JZNetPackManager::instance()->registPack<JZGpioSimPack>(NetPack_user + 2);
    s_gpioSimRegistered = true;
}
