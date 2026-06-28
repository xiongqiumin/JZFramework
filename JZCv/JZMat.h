#ifndef JZ_MAT_H_
#define JZ_MAT_H_

#include "JZSharedBuffer.h"

class JZMat
{
public:
    JZMat();
    ~JZMat();

protected:
    JZSharedBuffer m_buffer;
};

#endif