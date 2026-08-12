#ifndef JZ_WAVE_FILE_H_
#define JZ_WAVE_FILE_H_

#include "JZFile.h"
#include "JZAudioFormat.h"

// WAV 文件头结构（PCM 格式）
#pragma pack(push, 1)
struct WavHeader {
    // RIFF chunk
    char     riff_id[4];       // "RIFF"
    uint32_t riff_size;        // 文件大小 - 8
    char     wave_id[4];       // "WAVE"
    // fmt chunk
    char     fmt_id[4];        // "fmt "
    uint32_t fmt_size;         // fmt chunk 大小 (16 for PCM)
    uint16_t audio_format;     // 1 = PCM
    uint16_t num_channels;     // 声道数
    uint32_t sample_rate;      // 采样率
    uint32_t byte_rate;        // 每秒字节数
    uint16_t block_align;      // 每帧字节数
    uint16_t bits_per_sample;  // 位深
    // data chunk
    char     data_id[4];       // "data"
    uint32_t data_size;        // PCM 数据大小
};
#pragma pack(pop)

class JZWaveFile
{
public:
    JZWaveFile();
    ~JZWaveFile();

    // 打开 WAV 文件（支持资源路径如 :/audio/xxx.wav）
    bool open(const std::string& filepath);

    // 关闭文件
    void close();

    // 是否已打开
    bool isOpen() const;

    // 读取指定帧数的 PCM 数据（一帧 = 所有通道各一个采样点）
    JZByteArray read(int frames);

    // 读取全部 PCM 数据
    JZByteArray readAll();

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

    JZFile m_file;
    JZAudioFormat m_format;
    int m_dataSize;       // PCM 数据大小（字节）
    int m_dataOffset;     // PCM 数据在文件中的起始偏移
    int m_readPos;        // 当前读取位置（相对于数据起始）
};

#endif  // ! JZ_WAVE_FILE_H_
