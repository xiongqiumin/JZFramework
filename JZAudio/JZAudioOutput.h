#ifndef JZ_AUDIO_OUTPUT_H_
#define JZ_AUDIO_OUTPUT_H_

#include "JZObject.h"
#include "JZAudioFormat.h"
#include "JZAudioDevice.h"

class JZAudioOutputImpl;
class JZAudioOutput : public JZObject
{
public:
    JZAudioOutput(JZObject *parent = nullptr);
    ~JZAudioOutput();

    void setFormat(const JZAudioFormat& format);
    bool open();
    bool open(JZAudioDevice device);
    void close();
    void reset();
    bool write(const JZByteArray& data);

jz_signals:
    JZSigVoid sigPlayFinish;

protected:
    friend JZAudioOutputImpl;

    JZAudioOutputImpl* d;
    JZAudioFormat m_format;
};


#endif // !JZ_AUDIO_OUTPUT_H_