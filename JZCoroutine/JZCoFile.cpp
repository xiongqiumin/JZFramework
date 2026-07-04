#include <thread>
#include "JZCo.h"
#include "JZCoFile.h"

JZCoFile::JZCoFile()
{
}

JZCoFile::~JZCoFile()
{
}

JZByteArray JZCoFile::read(int max_size)
{
    JZByteArray recv;
    jzco_submitAndWait([this,&recv]{
        recv = m_file->read(max_size);
    });
    return recv;
}

JZByteArray JZCoFile::readAll()
{
    JZByteArray recv;
    jzco_submitAndWait([this,&recv]{
        recv = m_file->readAll();
    });
    return recv;
}