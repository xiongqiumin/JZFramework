#if JZ_OS == JZ_OS_WINDOWS
#include <windows.h>
#elif JZ_OS == JZ_OS_LINUX
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif 

#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZFileDriver.h"
#include "JZDebug.h"

bool JZFile::remove(std::string filepath)
{
#if JZ_OS == JZ_OS_WINDOWS
    std::string local_path = str_utf8_to_local(filepath);
    return DeleteFileA(local_path.c_str()) == TRUE;
#elif JZ_OS == JZ_OS_LINUX
    return unlink(filepath.c_str()) == 0;
#else
    auto *drv = JZFileAbstractDriver::instance();
    return drv ? drv->fileRemove(filepath) : false;
#endif
}

bool JZFile::exists(std::string filepath)
{
    return JZFileInfo(filepath).exists();
}

JZFile::JZFile()
{
    m_fp = nullptr;
}

JZFile::~JZFile()
{
    close();
}

bool JZFile::isOpen() const
{
    return m_fp != nullptr;
}

bool JZFile::open(std::string filepath, std::string mode)
{
    m_fp = fopen(str_utf8_to_local(filepath).c_str(), mode.c_str());
    return m_fp != nullptr;
}

void JZFile::close()
{
    if (m_fp)
    {
        fclose(m_fp);
        m_fp = nullptr;
    }
}

int JZFile::size()
{
    if (!m_fp)
        return 0;

    long cur = ftell(m_fp);
    if (cur == -1)
        return 0;

    if (fseek(m_fp, 0, SEEK_END) != 0)
        return 0;

    long size = ftell(m_fp);
    fseek(m_fp, cur, SEEK_SET);
    return size;
}

bool JZFile::atEnd() const
{
    if (!m_fp)
        return true;

    return feof(m_fp);
}

void JZFile::seek(int pos)
{
    fseek(m_fp, pos, SEEK_CUR);
}

void JZFile::seek(int pos, int set)
{
    fseek(m_fp, pos, set);
}

int JZFile::tell()
{
    return ftell(m_fp);
}

int JZFile::write(const JZByteArray &buffer)
{
    return write(buffer.data(), buffer.size());
}

int JZFile::write(const char *buffer, int size)
{
    int ret = (int)fwrite(buffer, 1, size, m_fp);
    if (ret != size)
    {
        jzError() << "JZFile::write failed: requested " << size << " bytes, actual written " << ret;
    }
    return ret;
}

JZByteArray JZFile::read(int max_size)
{
    JZByteArray result;
    result.resize(max_size);

    int size = read(result.data(), max_size);
    result.resize(size);
    return result;
}

int JZFile::read(char *buffer, int size)
{
    return (int)fread(buffer, 1, size, m_fp);
}

JZByteArray JZFile::readAll()
{
    JZByteArray ret;
    while (true)
    {
        JZByteArray tmp = read(32 * 1024);
        if (tmp.isEmpty())
            break;

        ret.append(tmp);
    }
    return ret;
}
