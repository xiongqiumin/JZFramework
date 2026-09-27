#ifndef JZ_AUDIO_OUTPUT_IMPL_H_
#define JZ_AUDIO_OUTPUT_IMPL_H_
#include "JZAudioDeviceInfo.h"
#include "JZObject.h"
#include "JZByteArray.h"

class JZAudioOutput;
class JZFRAMEWORK_API JZAudioOutputImpl : public JZObject
{
public:
    explicit JZAudioOutputImpl(JZAudioOutput* parent);
    virtual ~JZAudioOutputImpl();

    virtual bool open(const JZAudioDeviceInfo& device) = 0;
    virtual void close() = 0;
    virtual void reset() = 0;
    virtual bool write(const JZByteArray& data) = 0;

protected:
    JZAudioOutput *m_device;
};

#endif // !JZ_AUDIO_OUTPIT_IMPL_H_
