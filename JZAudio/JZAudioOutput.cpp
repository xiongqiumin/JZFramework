#include "JZAudioOutput.h"
#include "detail/JZAudioDriver.h"

JZAudioOutput::JZAudioOutput(JZObject* parent)
    : JZObject(parent), sigPlayFinish(this)
{
    d = JZAudioDriver::instance()->createOutput(this);
}

JZAudioOutput::~JZAudioOutput()
{
    close();
    delete d;
}

void JZAudioOutput::setFormat(const JZAudioFormat& format)
{
    m_format = format;
}

const JZAudioFormat& JZAudioOutput::format()
{
    return m_format;
}

bool JZAudioOutput::open()
{
    auto dev_list = JZAudioDeviceInfo::outputList();
    if(dev_list.size() == 0)
        return false;

    return d->open(dev_list[0]);
}

bool JZAudioOutput::open(JZAudioDeviceInfo device)
{
    return d->open(device);
}

void JZAudioOutput::close()
{
    d->close();
}

void JZAudioOutput::reset()
{
    d->reset();
}

bool JZAudioOutput::write(const JZByteArray& data)
{
    return d->write(data);
}