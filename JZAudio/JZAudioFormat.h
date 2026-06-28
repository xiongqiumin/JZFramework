#ifndef JZ_AUDIO_FORMAT_H_
#define JZ_AUDIO_FORMAT_H_

#include <string>
#include <vector>
#include "JZGlobal.h"

class JZAudioFormat
{
public:
    enum SampleFormat {
        UnknownSampleFormat = -1,
        UInt8 = 0,
        Int16 = 1,
        Int32 = 2,
        Float = 3
    };

	JZAudioFormat();

    int channelCount;
    int sampleRate;
    SampleFormat sampleFormat;
};

#endif // !JZ_AUDIO_FORMAT_H_
