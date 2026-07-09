#include "JZAudioInput.h"
#include "detail/JZAudioDriver.h"

JZAudioInput::JZAudioInput(JZObject* parent)
	:JZObject(parent), sigReadyRead(this)
{
	d = JZAudioDriver::instance()->createInput(this);
}

JZAudioInput::~JZAudioInput()
{
	close();
	delete d;
}

void JZAudioInput::setFormat(JZAudioFormat format)
{
	m_format = format;
}

const JZAudioFormat& JZAudioInput::format() const
{
	return m_format;
}

bool JZAudioInput::open()
{
	auto dev_list = JZAudioDeviceInfo::inputList();
    if(dev_list.size() == 0)
        return false;

    return d->open(dev_list[0]);
}

bool JZAudioInput::open(JZAudioDeviceInfo device)
{
	return d->open(device);
}

void JZAudioInput::close()
{ 
	d->close();
}