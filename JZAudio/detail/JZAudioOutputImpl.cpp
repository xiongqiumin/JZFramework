#include "JZAudioOutputImpl.h"
#include "JZAudioOutput.h"

JZAudioOutputImpl::JZAudioOutputImpl(JZAudioOutput* parent)
    :JZObject(parent)
{    
    m_device = parent;
}
    
JZAudioOutputImpl::~JZAudioOutputImpl()
{
}
