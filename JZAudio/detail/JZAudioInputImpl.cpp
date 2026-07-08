#include "JZAudioInputImpl.h"
#include "JZAudioInput.h"

JZAudioInputImpl::JZAudioInputImpl(JZAudioInput* parent)
    :JZObject(parent)
{    
    m_device = parent;
}
    
JZAudioInputImpl::~JZAudioInputImpl()
{
}
