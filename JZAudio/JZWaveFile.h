#ifndef JZ_WAVE_FILE_H_
#define JZ_WAVE_FILE_H_
#include "JZFile.h"
#include "JZAudioFormat.h"

class JZFRAMEWORK_API JZWaveFile
{
public:
    JZWaveFile();
    ~JZWaveFile();

    // 打开 WAV 文件（只读，支持资源路径如 :/audio/xxx.wav）
    bool open(const std::string& filepath);

    // 创建并打开 WAV 文件用于写入，按指定音频格式写入 PCM 数据
    bool create(const std::string& filepath, const JZAudioFormat& format);

    // 关闭文件（写入模式下会补齐并回写 WAV 头中的大小字段）
    void close();

    // 是否已打开
    bool isOpen() const;

    // 读取指定帧数的 PCM 数据（一帧 = 所有通道各一个采样点）
    JZByteArray read(int frames);

    // 读取全部 PCM 数据
    JZByteArray readAll();

    // 写入 PCM 数据（仅在 create() 写入模式下有效）
    // 每次写入后会自动回写 WAV 头中的大小字段，close() 时也会再次兜底更新
    int write(const JZByteArray& data);

    // 重置读取位置到数据起始处（用于循环播放）
    void reset();

    // 是否读到末尾
    bool atEnd() const;

    // 获取音频格式
    JZAudioFormat format() const;

    // 获取 PCM 数据总大小（字节）
    int dataSize() const;

private:
    bool parseHeader();
    bool writeHeader();

    JZFile m_file;
    JZAudioFormat m_format;
    int m_dataSize;       // PCM 数据大小（字节）
    int m_dataOffset;     // PCM 数据在文件中的起始偏移
    int m_readPos;        // 当前读取位置（相对于数据起始）
    bool m_writeMode;     // 是否为写入模式
};

#endif  // ! JZ_WAVE_FILE_H_
