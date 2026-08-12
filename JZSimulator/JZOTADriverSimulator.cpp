#include "JZOTADriverSimulator.h"
#include "JZSimPack.h"

JZOTADriverSimulator *JZOTADriverSimulator::simInstance()
{
    return dynamic_cast<JZOTADriverSimulator *>(JZOTADriver::instance());
}

void JZOTADriverSimulator::onRecv(JZSimPack *sp)
{
}

JZResult<void> JZOTADriverSimulator::verify(const JZOTAUpdateInfo &info)
{
    (void)info;
    return JZResult<void>();
}

int64_t JZOTADriverSimulator::totalSize()
{
    return -1;
}

bool JZOTADriverSimulator::begin(int64_t totalSize)
{
    (void)totalSize;
    return false;
}

int64_t JZOTADriverSimulator::write(int64_t offset, const JZByteArray &buffer)
{
    (void)offset;
    (void)buffer;
    return -1;
}

bool JZOTADriverSimulator::finish()
{
    return false;
}

void JZOTADriverSimulator::cancel()
{
}

void JZOTADriverSimulator::reboot()
{
}
