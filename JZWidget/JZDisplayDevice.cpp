#include "JZDisplayDevice.h"
#include "JZStd.h"

JZDisplayDevice::JZDisplayDevice(void* display)
    :JZWidget(NoCreate())
{}

JZDisplayDevice::JZDisplayDevice(int w,int h)
    :JZWidget(NoCreate())
{}

JZDisplayDevice::~JZDisplayDevice()
{    
#ifdef _WIN32

#endif
}

void* JZDisplayDevice::displayHandle() const
{
    return m_display;
}
