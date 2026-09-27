#ifndef JZ_FILE_H_
#define JZ_FILE_H_
#include "JZStd.h"
#include "JZByteArray.h"

struct JZResourceData;

class JZFRAMEWORK_API JZFile
{
public:
    static bool exists(std::string filepath);
    static bool remove(std::string filepath);

    // 拷贝文件（对应 QFile::copy）。分块读写，不把整个文件读进内存。
    // force: 目标文件已存在时是否覆盖。
    //   false(与 Qt 一致) -> 目标已存在直接失败，不覆盖；
    //   true             -> 覆盖。
    // 失败时错误信息写入 *error（可为 nullptr）。
    static bool copy(const std::string &from, const std::string &to,
                     bool force, std::string *error = nullptr);

    // 重命名 / 移动文件或目录（对应 QFile::rename）。目标已存在时覆盖。
    static bool rename(const std::string &from, const std::string &to,
                       std::string *error = nullptr);

    JZFile();
    ~JZFile();

    bool isOpen() const;
    bool open(std::string filepath, std::string mode);
    void close();

    // 把 stdio 缓冲区里的内容真正交给系统(fflush)。
    // ⚠ write() 只写进 stdio 的缓冲区: 不 flush 的话, 日志这种"写了但量不大"的内容
    //   会一直憋在内存里, 直到 close() 或攒满缓冲区 —— 进程被 kill 就全丢了。
    void flush();

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