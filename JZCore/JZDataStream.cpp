#include <string.h>
#include "JZDataStream.h"

//JZDataStream
JZDataStream::JZDataStream(JZFile *file)
{
    m_file = file;
    m_offset = 0;
    m_buffer = nullptr;
    m_error = false;
    m_errorString.clear();
    m_maxSize = -1;
}

JZDataStream::JZDataStream(const JZByteArray *buffer)
{
    m_buffer = (JZByteArray*)buffer;
    m_offset = 0;
    m_file = nullptr;
    m_error = false;
    m_errorString.clear();
    m_maxSize = -1;
}

JZDataStream::JZDataStream(JZByteArray *buffer, JZ::OpenMode mode)
{
    m_buffer = buffer;
    m_offset = 0;
    m_file = nullptr;
    m_error = false;
    m_errorString.clear();
    m_maxSize = -1;
}

JZDataStream::~JZDataStream()
{
}

void JZDataStream::setError(const std::string& msg)
{
    m_error = true;
    m_errorString = msg;
}

void JZDataStream::clearError()
{
    m_error = false;
    m_errorString.clear();
}

bool JZDataStream::checkReadCapacity(int64_t need) const
{
    if (need < 0)
        return false;
    if (m_file)
        return true;   // 文件流不预判容量
    if (!m_buffer)
        return false;
    int64_t rem = (int64_t)m_buffer->size() - (int64_t)m_offset;
    return need <= rem;
}

int JZDataStream::writeRawData(const char *s, int len)
{
    if (m_error)
        return 0;
    if (m_file)
    {
        len = m_file->write(s, len);
    }
    else
    {
        if (m_maxSize >= 0 && (int64_t)m_offset + len > m_maxSize)
        {
            setError("JZDataStream: writeRawData exceeds max size");
            return 0;
        }
        if (m_offset + len > m_buffer->size())
            m_buffer->resize(m_offset + len);

        memcpy(m_buffer->data() + m_offset, s, len);
        m_offset += len;
    }
    return len;
}

int JZDataStream::readRawData(char *s, int len)
{
    if (m_error)
        return 0;
    if (m_file)
    {
        len = m_file->read(s, len);
    }
    else
    {
        if (m_offset + len > m_buffer->size())
        {
            // 内存流不足: 设置错误, 不再读取 (避免越界/截断)
            setError("JZDataStream: readRawData out of range");
            return 0;
        }

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
    if (m_error)
        return;
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
    if (s.isError())
        return s;
    s << (int32_t)str.size();
    s.writeRawData(str.data(), (int)str.size());
    return s;
}

JZDataStream &operator>>(JZDataStream &s, std::string &str)
{
    if (s.isError())
        return s;
    int32_t size;
    s >> size;
    if (s.isError())
        return s;
    if (size < 0)
    {
        s.setError("JZDataStream: invalid string size");
        return s;
    }
    if (!s.checkReadCapacity((int64_t)size))
    {
        s.setError("JZDataStream: string size exceeds available buffer");
        return s;
    }
    str.resize(size);
    s.readRawData(&str[0], size);
    return s;
}

JZDataStream &operator<<(JZDataStream &s, const JZByteArray &buffer)
{
    if (s.isError())
        return s;
    s << (int32_t)buffer.size();
    s.writeRawData(buffer.data(), buffer.size());
    return s;
}

JZDataStream &operator>>(JZDataStream &s, JZByteArray &buffer)
{
    if (s.isError())
        return s;
    int32_t size;
    s >> size;
    if (s.isError())
        return s;
    if (size < 0)
    {
        s.setError("JZDataStream: invalid buffer size");
        return s;
    }
    if (!s.checkReadCapacity((int64_t)size))
    {
        s.setError("JZDataStream: buffer size exceeds available buffer");
        return s;
    }
    buffer.resize(size);
    s.readRawData(buffer.data(), size);
    return s;
}