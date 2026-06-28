#include <string.h>
#include "JZSharedBuffer.h"

//JZSharedBufferData
JZSharedBufferData::JZSharedBufferData()
{
    isOwner = false;
    buffer = nullptr;
    size = 0;
}

JZSharedBufferData::~JZSharedBufferData()
{
    release();
}

void JZSharedBufferData::alloc(int alloc_size)
{
    release();
    buffer = (char*)malloc(alloc_size);
    isOwner = true;
    size = alloc_size;
}

void JZSharedBufferData::release()
{
    if (isOwner)
        free(buffer);

    isOwner = false;
    buffer = nullptr;
    size = 0;
}

//JZSharedBuffer
JZSharedBuffer JZSharedBuffer::fromRaw(char *ptr, int len)
{
    JZSharedBuffer buffer;
    buffer.m_buffer->isOwner = false;
    buffer.m_buffer->buffer = (char*)ptr;
    buffer.m_buffer->size = len;
    return buffer;
}

JZSharedBuffer::JZSharedBuffer()
{
    m_buffer = std::make_shared<JZSharedBufferData>();
}

JZSharedBuffer::~JZSharedBuffer()
{
}

JZSharedBuffer JZSharedBuffer::clone() const
{
    JZSharedBuffer ret;
    ret.alloc(m_buffer->size);
    memcpy(ret.data(), data(), m_buffer->size);
    return ret;
}

void JZSharedBuffer::alloc(int size)
{
    m_buffer->alloc(size);
}

void JZSharedBuffer::release()
{
    m_buffer->release();
}

char *JZSharedBuffer::data()
{
    return m_buffer->buffer;
}

const char *JZSharedBuffer::data() const
{
    return m_buffer->buffer;
}

int JZSharedBuffer::size() const
{
    return m_buffer->size;
}