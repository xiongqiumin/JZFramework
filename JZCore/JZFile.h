#ifndef JZ_FILE_H_
#define JZ_FILE_H_

#include "JZStd.h"
#include "JZByteArray.h"

class JZFile
{
public:
    static bool exists(string filepath);
    static bool remove(string filepath);

    JZFile();
    ~JZFile();

    bool isOpen() const;
    bool open(string filepath,string mode);
    void close();

    bool atEnd() const;
    void seek(int pos);
    void seek(int pos,int origin);
    int tell();
    int size();

    JZByteArray read(int max_size);
    int read(char *buffer, int size);
    JZByteArray readAll();
    
    int write(const JZByteArray &buffer);
    int write(const char *buffer, int size);           

protected:
    JZ_DISABLE_COPY(JZFile)

    FILE *m_fp;
};

#endif