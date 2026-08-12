#ifndef JZ_FILE_H_
#define JZ_FILE_H_

#include "JZStd.h"
#include "JZByteArray.h"

struct JZResourceData;

class JZFile
{
public:
    static bool exists(std::string filepath);
    static bool remove(std::string filepath);

    JZFile();
    ~JZFile();

    bool isOpen() const;
    bool open(std::string filepath, std::string mode);
    void close();

    bool atEnd() const;
    void seek(int pos);
    void seek(int pos, int origin);
    int tell();
    int size();

    JZByteArray read(int max_size);
    int read(char *buffer, int size);
    JZByteArray readAll();
    
    int write(const JZByteArray &buffer);
    int write(const char *buffer, int size);

protected:
    JZ_DISABLE_COPY(JZFile)

#ifdef JZ_OS_NORMAL
    FILE *m_fp = nullptr;
#else
    void *m_fp = nullptr;
#endif
    const JZResourceData *m_res = nullptr;
    int m_resOffset = 0;
};

#endif