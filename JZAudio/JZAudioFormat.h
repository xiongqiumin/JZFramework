#ifndef JZ_AUDIO_FORMAT_H_
#define JZ_AUDIO_FORMAT_H_
#include <string>
#include <vector>
#include "JZGlobal.h"

class JZFRAMEWORK_API JZAudioFormat
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

    int bytesPerSample() const {
        switch (sampleFormat) {
        case UInt8:  return 1;
        case Int16:  return 2;
        case Int32:
        case Float:  return 4;
        default:     return 0;
        }
    }
};

#endif // !JZ_AUDIO_FORMAT_H_
