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
#include "JZResources.h"
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
    if (!filepath.empty() && filepath[0] == ':')
    {
        return JZResources::instance()->find(filepath) != nullptr;
    }
    return JZFileInfo(filepath).exists();
}

JZFile::JZFile() {}

JZFile::~JZFile()
{
    close();
}

bool JZFile::isOpen() const
{
    return m_fp != nullptr || m_res != nullptr;
}

bool JZFile::open(std::string filepath, std::string mode)
{
    if (!filepath.empty() && filepath[0] == ':')
    {
        // 资源文件只读，拒绝 a/w 模式
        if (mode.find_first_of("aw") != std::string::npos)
            return false;
        m_res = JZResources::instance()->find(filepath);
        m_resOffset = 0;
        return m_res != nullptr;
    }
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
    m_res = nullptr;
    m_resOffset = 0;
}

int JZFile::size()
{
    if (m_res)
        return m_res->size;

    if (!m_fp)
        return 0;

    long cur = ftell(m_fp);
    if (cur == -1)
        return 0;

    if (fseek(m_fp, 0, SEEK_END) != 0)
        return 0;

    long sz = ftell(m_fp);
    fseek(m_fp, cur, SEEK_SET);
    return (int)sz;
}

bool JZFile::atEnd() const
{
    if (m_res)
        return m_resOffset >= m_res->size;
    if (!m_fp)
        return true;
    return feof(m_fp);
}

void JZFile::seek(int pos)
{
    if (m_res)
    {
        m_resOffset += pos;
        if (m_resOffset < 0) 
            m_resOffset = 0;
        if (m_resOffset > m_res->size) 
            m_resOffset = m_res->size;
        return;
    }
    fseek(m_fp, pos, SEEK_CUR);
}

void JZFile::seek(int pos, int origin)
{
    if (m_res)
    {
        switch (origin)
        {
            case SEEK_SET: m_resOffset = pos; break;
            case SEEK_CUR: m_resOffset += pos; break;
            case SEEK_END: m_resOffset = m_res->size + pos; break;
        }
        if (m_resOffset < 0) 
            m_resOffset = 0;
        if (m_resOffset > m_res->size) 
            m_resOffset = m_res->size;
        return;
    }
    fseek(m_fp, pos, origin);
}

int JZFile::tell()
{
    if (m_res)
        return m_resOffset;
    return ftell(m_fp);
}

int JZFile::write(const JZByteArray &buffer)
{
    return write(buffer.data(), buffer.size());
}

int JZFile::write(const char *buffer, int size)
{
    if (m_res)
        return 0; // 资源只读
        
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

    int sz = read(result.data(), max_size);
    result.resize(sz);
    return result;
}

int JZFile::read(char *buffer, int size)
{
    if (m_res)
    {
        int remain = m_res->size - m_resOffset;
        if (remain <= 0) return 0;
        int to_read = size < remain ? size : remain;
        memcpy(buffer, m_res->data + m_resOffset, to_read);
        m_resOffset += to_read;
        return to_read;
    }
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
