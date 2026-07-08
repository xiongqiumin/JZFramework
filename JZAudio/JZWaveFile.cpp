#include "JZWaveFile.h"
#include "JZDebug.h"
#include <cstring>

JZWaveFile::JZWaveFile()
    : m_dataSize(0)
    , m_dataOffset(0)
    , m_readPos(0)
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

    m_readPos = 0;
    return true;
}

void JZWaveFile::close()
{
    if (m_file.isOpen()) {
        m_file.close();
    }
    m_dataSize = 0;
    m_dataOffset = 0;
    m_readPos = 0;
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