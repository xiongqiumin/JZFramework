#include "JZAudioInput.h"
#include "JZAudioInputImpl.h"

JZAudioInput::JZAudioInput(JZObject* parent)
	:JZObject(parent), sigReadyRead(this)
{
	d = new JZAudioInputImpl(this);
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

bool JZAudioInput::open()
{
	auto dev_list = JZAudioDevice::audioInputList();
    if(dev_list.size() == 0)
        return false;

    return d->open(dev_list[0]);
}

bool JZAudioInput::open(JZAudioDevice device)
{
	return d->open(device);
}

void JZAudioInput::close()
{ 
	d->close();
}