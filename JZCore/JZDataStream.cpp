#include <string.h>
#include "JZDataStream.h"

//JZDataStream
JZDataStream::JZDataStream(JZFile *file)
{
    m_file = file;
    m_offset = 0;
    m_buffer = nullptr;
}

JZDataStream::JZDataStream(const JZByteArray &buffer)
{
    m_buffer = (JZByteArray*)&buffer;
    m_offset = 0;
    m_file = nullptr;
}

JZDataStream::JZDataStream(JZByteArray *buffer, JZ::OpenMode mode)
{
    m_buffer = buffer;
    m_offset = 0;
    m_file = nullptr;
}

JZDataStream::~JZDataStream()
{
}

int JZDataStream::writeRawData(const char *s, int len)
{
    if (m_file)
    {
        len = m_file->write(s, len);
    }
    else
    {
        if (m_offset + len > m_buffer->size())
            m_buffer->resize(m_offset + len);

        memcpy(m_buffer->data() + m_offset, s, len);
        m_offset += len;
    }
    return len;
}

int JZDataStream::readRawData(char *s, int len)
{
    if (m_file)
    {
        len = m_file->read(s, len);
    }
    else
    {
        if (m_offset + len > m_buffer->size())
            len = m_buffer->size() - m_offset;

        if (len > 0)
        {
            memcpy(s, m_buffer->data() + m_offset, len);
            m_offset += len;
        }
    }
    return len;
}

void JZDataStream::skipRawData(int offset)
{
    if (m_file)
        m_file->seek(offset);
    else
        m_offset += offset;
}

void JZDataStream::setByteOrder(ByteOrder order)
{

}

JZDataStream::ByteOrder JZDataStream::byteOrder()
{
    return JZDataStream::LittleEndian;
}

JZDataStream &JZDataStream::operator<<(bool i)
{
    writePod<bool>(i);
    return *this;
}

JZDataStream &JZDataStream::operator>>(bool &i)
{    
    readPod<bool>(i);
    return *this;
}

JZDataStream &JZDataStream::operator<<(int8_t i)
{
    writePod<int8_t>(i);
    return *this;
}

JZDataStream &JZDataStream::operator>>(int8_t &i)
{    
    readPod<int8_t>(i);
    return *this;
}

JZDataStream &JZDataStream::operator<<(uint8_t i)
{
    writePod<uint8_t>(i);
    return *this;
}

JZDataStream &JZDataStream::operator>>(uint8_t &i)
{    
    readPod<uint8_t>(i);
    return *this;
}

JZDataStream &JZDataStream::operator<<(int16_t i)
{
    writePod<int16_t>(i);
    return *this;
}

JZDataStream &JZDataStream::operator>>(int16_t &i)
{    
    readPod<int16_t>(i);
    return *this;
}

JZDataStream &JZDataStream::operator<<(uint16_t i)
{
    writePod<uint16_t>(i);
    return *this;
}

JZDataStream &JZDataStream::operator>>(uint16_t &i)
{    
    readPod<uint16_t>(i);
    return *this;
}

JZDataStream &JZDataStream::operator<<(int32_t i)
{
    writePod<int32_t>(i);
    return *this;        
}

JZDataStream &JZDataStream::operator>>(int32_t &i)
{
    readPod<int32_t>(i);
    return *this;
}

JZDataStream &JZDataStream::operator<<(uint32_t i)
{
    writePod<uint32_t>(i);
    return *this;
}

JZDataStream &JZDataStream::operator>>(uint32_t &i)
{    
    readPod<uint32_t>(i);
    return *this;
}

JZDataStream &JZDataStream::operator<<(int64_t i)
{
    writePod<int64_t>(i);
    return *this;
}

JZDataStream &JZDataStream::operator>>(int64_t &i)
{    
    readPod<int64_t>(i);
    return *this;
}

JZDataStream &JZDataStream::operator<<(uint64_t i)
{
    writePod<uint64_t>(i);
    return *this;
}

JZDataStream &JZDataStream::operator>>(uint64_t &i)
{    
    readPod<uint64_t>(i);
    return *this;
}

JZDataStream &JZDataStream::operator<<(float i)
{
    writePod<float>(i);
    return *this;        
}

JZDataStream &JZDataStream::operator>>(float &i)
{
    readPod<float>(i);
    return *this;
}

JZDataStream &JZDataStream::operator<<(double i)
{
    writePod<double>(i);
    return *this;        
}

JZDataStream &JZDataStream::operator>>(double &i)
{
    readPod<double>(i);
    return *this;
}

JZDataStream &operator<<(JZDataStream &s, const std::string &str)
{
    s << (int32_t)str.size();
    s.writeRawData(str.data(), (int)str.size());
    return s;
}

JZDataStream &operator>>(JZDataStream &s, std::string &str)
{
    int32_t size;
    s >> size;
    str.resize(size);
    s.readRawData(&str[0],size);
    return s;
}

JZDataStream &operator<<(JZDataStream &s, const JZByteArray &buffer)
{
    s << (int32_t)buffer.size();
    s.writeRawData(buffer.data(), buffer.size());
    return s;
}

JZDataStream &operator>>(JZDataStream &s, JZByteArray &buffer)
{
    int32_t size;
    s >> size;
    buffer.resize(size);
    s.readRawData(buffer.data(), size);
    return s;
}