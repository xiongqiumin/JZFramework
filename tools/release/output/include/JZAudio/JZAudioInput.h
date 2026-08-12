#ifndef JZAUDIO_INPUT_H_
#define JZAUDIO_INPUT_H_

#include "JZObject.h"
#include "JZAudioFormat.h"
#include "JZByteArray.h"
#include "JZAudioDeviceInfo.h"

class JZAudioInputImpl;
class JZAudioInput : public JZObject
{
public:
	JZAudioInput(JZObject *parent = nullptr);
	~JZAudioInput();

	void setFormat(JZAudioFormat format);
	const JZAudioFormat &format() const;

	bool open();
	bool open(JZAudioDeviceInfo device);
	void close();

jz_signals:
	JZSigByteArray sigReadyRead;

protected:
	friend JZAudioInputImpl;

	JZAudioFormat m_format;
	JZAudioInputImpl* d;
};



#endif // !JZAUDIO_INPUT_H_