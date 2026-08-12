#ifndef JZ_WAVE_UTILS_H_
#define JZ_WAVE_UTILS_H_

#include "JZByteArray.h"
#include "JZAudioFormat.h"

JZByteArray audioTrans(const JZByteArray& input, JZAudioFormat input_format, JZAudioFormat output_format);

#endif  // JZ_WAVE_UTILS_H_
