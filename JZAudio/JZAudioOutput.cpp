#include "JZAudioOutput.h"
#include "JZAudioOutputImpl.h"

JZAudioOutput::JZAudioOutput(JZObject* parent)
    : JZObject(parent), sigPlayFinish(this), d(new JZAudioOutputImpl(this))
{
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


bool JZAudioOutput::open()
{
    auto dev_list = JZAudioDevice::audioOutputList();
    if(dev_list.size() == 0)
        return false;

    return d->open(dev_list[0]);
}

bool JZAudioOutput::open(JZAudioDevice device)
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