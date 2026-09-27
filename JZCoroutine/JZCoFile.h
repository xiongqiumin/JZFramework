#ifndef JZCO_FILE_H
#define JZCO_FILE_H
#include "JZFile.h"

class JZFRAMEWORK_API JZCoFile
{
public:
    JZCoFile();
    ~JZCoFile();

    JZByteArray read(int max_size);
    JZByteArray readAll();

protected:
    JZFile m_file;
};

#endif // !JZCO_NET_WORK_H
