#include "JZTextStream.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

const size_t JZTextStream::BUFFER_SIZE = 4096;

JZTextStream::JZTextStream(JZFile* file)
{
    m_codec = UTF8;
    m_file = file;
    if (file->size() == 0)
        m_bomWritten = false;
    else
    {
        m_bomWritten = true;
        skipUTF8BOM();
    }

    // 预分配缓冲区大小，避免频繁扩容
    buf_pos = 0;
    read_buffer.reserve(BUFFER_SIZE);
    line_buf.reserve(BUFFER_SIZE);
}

JZTextStream::~JZTextStream()
{
}

void JZTextStream::skipUTF8BOM()
{
    JZByteArray bom = m_file->read(3);
    size_t readCount = bom.size();

    bool isUTF8BOM = (readCount == 3) && (bom[0] == (char)0xEF) && (bom[1] == (char)0xBB) && (bom[2] == (char)0xBF);

    if (!isUTF8BOM)
    {
        m_file->seek(0, SEEK_SET);
    }
    else
    {
        m_codec = UTF8_BOM;
    }
}

// 读取全部内容
std::string JZTextStream::readAll()
{
    return str_fromUtf8(m_file->readAll());
}

// 逐行读取（使用string作为缓冲区）
std::string JZTextStream::readLine()
{
    line_buf.clear();  // 清空行缓冲区
    while (true)
    {
        if (buf_pos >= read_buffer.size())
        {
            auto byte_array = m_file->read(BUFFER_SIZE);
            read_buffer = str_fromUtf8(byte_array);

            buf_pos = 0;
            if (read_buffer.empty())
            {
                return line_buf.empty() ? "" : line_buf;
            }
        }

        // 遍历缓冲区中的字符
        while (buf_pos < read_buffer.size())
        {
            char ch = read_buffer[buf_pos++];
            if (ch == '\n')
            {
                return line_buf;
            }
            else if (ch == '\r')
            {
                if (buf_pos < read_buffer.size() && read_buffer[buf_pos] == '\n')
                {
                    buf_pos++;
                }
                return line_buf;
            }
            else
            {
                line_buf += ch;
            }
        }
    }
}

bool JZTextStream::atEnd() const
{
    return buf_pos >= read_buffer.size() && m_file->atEnd();
}

void JZTextStream::setCodec(Codec codec)
{
    m_codec = codec;
}

JZTextStream::Codec JZTextStream::codec() const
{
    return m_codec;
}

// 自动写 BOM（只在第一次写入时调用）
void JZTextStream::writeBomIfNeeded()
{
    if (m_bomWritten)
        return;

    m_bomWritten = true;

    // 只有 UTF8_BOM 才写 BOM
    if (m_codec == UTF8_BOM && m_file)
    {
        m_file->write((const char *)UTF8_BOM, 3);
    }
}

// 写入字符串
JZTextStream &JZTextStream::operator<<(const std::string &str)
{
    if (!m_file)
        return *this;

    writeBomIfNeeded();
    if(m_codec == Local)
    {
        std::string local = str_utf8_to_local(str);
        m_file->write(local.data(), (int)local.size());
    }
    else
    {
        m_file->write(str.data(), (int)str.size());
    }
    return *this;
}