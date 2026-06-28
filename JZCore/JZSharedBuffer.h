#ifndef JZ_SHARED_BUFFER_H_
#define JZ_SHARED_BUFFER_H_

#include <memory>
#include "JZGlobal.h"
#include "JZDataStream.h"

//JZSharedBufferData
class JZSharedBufferData
{
public:
    JZSharedBufferData();
    ~JZSharedBufferData();

    void alloc(int size);
    void release();

    bool isOwner;
    char *buffer;
    int size;
};

//JZSharedBuffer
class JZSharedBuffer
{
public:
    static JZSharedBuffer fromRaw(char *buffer,int len);
    
    JZSharedBuffer();    
    ~JZSharedBuffer();

    JZSharedBuffer clone() const;
    void alloc(int size);
    void release();

    char *data();
    const char *data() const;
    int size() const;
    
protected:
    std::shared_ptr<JZSharedBufferData> m_buffer;
};

#endif