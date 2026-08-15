#include "JZWaveFile.h"
#include "JZDebug.h"
#include <cstring>
#include <cstdint>

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

JZWaveFile::JZWaveFile()
    : m_dataSize(0)
    , m_dataOffset(0)
    , m_readPos(0)
    , m_writeMode(false)
{
}

JZWaveFile::~JZWaveFile()
{
    close();
}

bool JZWaveFile::open(const std::string& filepath)
{
    close();

    if (!m_file.open(filepath, "rb")) {
        jzDebug() << "JZWaveFile: failed to open" << filepath;
        return false;
    }

    if (!parseHeader()) {
        jzDebug() << "JZWaveFile: failed to parse WAV header";
        close();
        return false;
    }

    m_writeMode = false;
    m_readPos = 0;
    return true;
}

bool JZWaveFile::create(const std::string& filepath, const JZAudioFormat& format)
{
    close();

    m_format = format;

    int bytesPerSample = m_format.bytesPerSample();
    if (m_format.channelCount <= 0 || m_format.sampleRate <= 0 || bytesPerSample <= 0) {
        jzDebug() << "JZWaveFile: invalid audio format for writing";
        return false;
    }

    if (!m_file.open(filepath, "wb")) {
        jzDebug() << "JZWaveFile: failed to open for writing" << filepath;
        return false;
    }

    // 写入模式下先写入占位头，待数据写完后回写大小字段
    m_dataSize = 0;
    m_dataOffset = sizeof(WavHeader);
    m_readPos = 0;
    m_writeMode = true;

    if (!writeHeader()) {
        jzDebug() << "JZWaveFile: failed to write WAV header";
        close();
        return false;
    }

    return true;
}

void JZWaveFile::close()
{
    if (m_file.isOpen()) {
        if (m_writeMode) {
            // 回写 WAV 头中的大小字段（data_size 与 riff_size）
            if (!writeHeader()) {
                jzDebug() << "JZWaveFile: failed to update WAV header on close";
            }
        }
        m_file.close();
    }
    m_dataSize = 0;
    m_dataOffset = 0;
    m_readPos = 0;
    m_writeMode = false;
}

bool JZWaveFile::isOpen() const
{
    return m_file.isOpen();
}

bool JZWaveFile::parseHeader()
{
    // 读取完整的 WAV 头
    JZByteArray headerData = m_file.read(sizeof(WavHeader));
    if (headerData.size() < (int)sizeof(WavHeader)) {
        jzDebug() << "JZWaveFile: file too small for WAV header";
        return false;
    }

    const WavHeader* hdr = reinterpret_cast<const WavHeader*>(headerData.data());

    // 校验 RIFF 和 WAVE 标识
    if (std::memcmp(hdr->riff_id, "RIFF", 4) != 0 ||
        std::memcmp(hdr->wave_id, "WAVE", 4) != 0) {
        jzDebug() << "JZWaveFile: not a valid WAV file";
        return false;
    }

    // 校验 fmt chunk
    if (std::memcmp(hdr->fmt_id, "fmt ", 4) != 0) {
        jzDebug() << "JZWaveFile: fmt chunk not found";
        return false;
    }

    // 仅支持 PCM 格式
    if (hdr->audio_format != 1) {
        jzDebug() << "JZWaveFile: only PCM format supported, got" << hdr->audio_format;
        return false;
    }

    // 设置音频格式
    m_format.channelCount = hdr->num_channels;
    m_format.sampleRate = hdr->sample_rate;

    switch (hdr->bits_per_sample) {
    case 8:
        m_format.sampleFormat = JZAudioFormat::UInt8;
        break;
    case 16:
        m_format.sampleFormat = JZAudioFormat::Int16;
        break;
    case 32:
        m_format.sampleFormat = JZAudioFormat::Int32;
        break;
    default:
        jzDebug() << "JZWaveFile: unsupported bits_per_sample" << hdr->bits_per_sample;
        return false;
    }

    // 校验 data chunk
    if (std::memcmp(hdr->data_id, "data", 4) != 0) {
        jzDebug() << "JZWaveFile: data chunk not found";
        return false;
    }

    m_dataSize = hdr->data_size;
    m_dataOffset = sizeof(WavHeader);

    jzDebug() << "JZWaveFile: opened,"
              << "channels=" << m_format.channelCount
              << "sampleRate=" << m_format.sampleRate
              << "bits=" << hdr->bits_per_sample
              << "dataSize=" << m_dataSize;

    return true;
}

bool JZWaveFile::writeHeader()
{
    int bytesPerSample = m_format.bytesPerSample();
    int bitsPerSample = bytesPerSample * 8;
    int blockAlign = m_format.channelCount * bytesPerSample;
    int byteRate = m_format.sampleRate * blockAlign;
    uint32_t riffSize = (uint32_t)(sizeof(WavHeader) - 8 + m_dataSize);

    WavHeader hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    std::memcpy(hdr.riff_id, "RIFF", 4);
    hdr.riff_size = riffSize;
    std::memcpy(hdr.wave_id, "WAVE", 4);
    std::memcpy(hdr.fmt_id, "fmt ", 4);
    hdr.fmt_size = 16;
    hdr.audio_format = 1;  // PCM
    hdr.num_channels = (uint16_t)m_format.channelCount;
    hdr.sample_rate = (uint32_t)m_format.sampleRate;
    hdr.byte_rate = (uint32_t)byteRate;
    hdr.block_align = (uint16_t)blockAlign;
    hdr.bits_per_sample = (uint16_t)bitsPerSample;
    std::memcpy(hdr.data_id, "data", 4);
    hdr.data_size = (uint32_t)m_dataSize;

    // 跳转回文件头写入（写入模式）或仅在打开时写入（data_size=0）
    m_file.seek(0, SEEK_SET);
    JZByteArray header(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    int written = m_file.write(header);
    if (written != (int)sizeof(hdr)) {
        jzDebug() << "JZWaveFile: failed to write full header";
        return false;
    }

    // 写完后定位到数据区末尾，以便后续追加写入
    m_file.seek(m_dataOffset + m_dataSize);
    return true;
}

JZByteArray JZWaveFile::read(int frames)
{
    if (!isOpen()) {
        return JZByteArray();
    }

    int remaining = m_dataSize - m_readPos;
    if (remaining <= 0) {
        return JZByteArray();
    }

    // 每帧字节数 = 通道数 × 位深/8
    int bytesPerFrame = m_format.channelCount * (m_format.bytesPerSample());
    int bytes = frames * bytesPerFrame;

    int toRead = (bytes < remaining) ? bytes : remaining;

    // 定位到当前读取位置
    m_file.seek(m_dataOffset + m_readPos);
    JZByteArray data = m_file.read(toRead);
    m_readPos += data.size();

    return data;
}

JZByteArray JZWaveFile::readAll()
{
    if (!isOpen()) {
        return JZByteArray();
    }

    int remaining = m_dataSize - m_readPos;
    if (remaining <= 0) {
        return JZByteArray();
    }

    m_file.seek(m_dataOffset + m_readPos);
    JZByteArray data = m_file.read(remaining);
    m_readPos += data.size();

    return data;
}

int JZWaveFile::write(const JZByteArray& data)
{
    if (!isOpen() || !m_writeMode) {
        jzDebug() << "JZWaveFile: write called without opening in write mode";
        return 0;
    }

    int written = m_file.write(data);
    if (written > 0) {
        m_dataSize += written;
        // 每写一批数据就回写 data_size，避免最后忘记 close 时头信息不准
        m_file.seek(0);
        // 仅更新头部大小字段：复用 writeHeader 会定位到数据末尾，正好符合要求
        writeHeader();
    }
    return written;
}

void JZWaveFile::reset()
{
    m_readPos = 0;
}

bool JZWaveFile::atEnd() const
{
    return m_readPos >= m_dataSize;
}

JZAudioFormat JZWaveFile::format() const
{
    return m_format;
}

int JZWaveFile::dataSize() const
{
    return m_dataSize;
}