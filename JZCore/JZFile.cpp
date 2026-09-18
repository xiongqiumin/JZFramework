#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZResources.h"
#include "JZDebug.h"
#include "JZPlatform.h"
#include "string.h"

#if JZ_OS == JZ_OS_WINDOWS
#include <windows.h>
#elif JZ_OS == JZ_OS_LINUX
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

bool JZFile::remove(std::string filepath)
{
#if JZ_OS == JZ_OS_WINDOWS
    std::string local_path = str_utf8_to_local(filepath);
    return DeleteFileA(local_path.c_str()) == TRUE;
#elif JZ_OS == JZ_OS_LINUX
    return unlink(filepath.c_str()) == 0;
#else
    return JZPlatform::instance()->fileRemove(filepath);
#endif
}

bool JZFile::exists(std::string filepath)
{
    if (JZFileInfo::isResourcePath(filepath))
    {
        return JZResources::instance()->find(filepath) != nullptr;
    }
    return JZFileInfo(filepath).exists();
}

bool JZFile::copy(const std::string &from, const std::string &to,
                  bool force, std::string *error)
{
    if (error)
        error->clear();

    if (!JZFile::exists(from))
    {
        if (error) *error = "源文件不存在: " + from;
        return false;
    }
    if (JZFileInfo(from).isDir())
    {
        if (error) *error = "源是目录(拷贝目录请用 JZDir::copyRecursively): " + from;
        return false;
    }
    if (JZFile::exists(to))
    {
        if (JZFileInfo(to).isDir())
        {
            if (error) *error = "目标是目录: " + to;
            return false;
        }
        if (!force)
        {
            if (error) *error = "目标文件已存在(force=false): " + to;
            return false;
        }
    }

    JZFile in, out;
    if (!in.open(from, "rb"))
    {
        if (error) *error = "打开源文件失败: " + from;
        return false;
    }
    if (!out.open(to, "wb"))
    {
        if (error) *error = "创建目标文件失败: " + to;
        return false;
    }

    const int kChunkSize = 64 * 1024;
    bool ok = true;
    while (true)
    {
        JZByteArray buf = in.read(kChunkSize);
        if (buf.isEmpty())
            break;
        if (out.write(buf) != buf.size())
        {
            if (error) *error = "写入目标文件失败: " + to;
            ok = false;
            break;
        }
    }
    in.close();
    out.close();
    return ok;
}

bool JZFile::rename(const std::string &from, const std::string &to, std::string *error)
{
    if (error)
        error->clear();

    if (!JZFile::exists(from))
    {
        if (error) *error = "源不存在: " + from;
        return false;
    }

    bool ok = false;
#if JZ_OS == JZ_OS_WINDOWS
    // MOVEFILE_REPLACE_EXISTING: 目标已存在时覆盖；MOVEFILE_COPY_ALLOWED: 跨盘也能移动
    ok = MoveFileExA(str_utf8_to_local(from).c_str(), str_utf8_to_local(to).c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) == TRUE;
#elif JZ_OS == JZ_OS_LINUX
    ok = ::rename(from.c_str(), to.c_str()) == 0;
#else
    ok = false;
    if (error) *error = "当前平台不支持重命名: " + from;
    return false;
#endif

    if (!ok && error)
        *error = "重命名失败: " + from + " -> " + to;
    return ok;
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
    if (m_fp)
    {
        jzWarning() << "allready open";
        return false;
    }

    if (JZFileInfo::isResourcePath(filepath))
    {
        if (mode.find_first_of("aw") != std::string::npos)
        {
            jzWarning() << "JZFile::open failed: resource path " << filepath
                        << " is read-only, mode \"" << mode << "\" not allowed";
            return false;
        }

        m_res = JZResources::instance()->find(filepath);
        m_resOffset = 0;
        return m_res != nullptr;
    }

#ifdef JZ_OS_NORMAL
    m_fp = fopen(str_utf8_to_local(filepath).c_str(), mode.c_str());
    return m_fp != nullptr;
#else
    m_fp = JZPlatform::instance()->fopen(filepath.c_str(), mode.c_str());
    return m_fp != nullptr;
#endif
}

void JZFile::flush()
{
    // 平台实例的 fopen 返回的同样是 FILE*, 所以两边都能直接 fflush
    if (m_fp)
        fflush(m_fp);
}

void JZFile::close()
{
    if (m_fp)
    {
#ifdef JZ_OS_NORMAL
        fclose(m_fp);
#else
        JZPlatform::instance()->fclose(m_fp);
#endif
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
    {
        jzWarning() << "JZFile::size failed: file not open";
        return 0;
    }

#ifdef JZ_OS_NORMAL
    long cur = ftell(m_fp);
    if (cur == -1)
    {
        jzWarning() << "JZFile::size failed: ftell error";
        return 0;
    }

    if (fseek(m_fp, 0, SEEK_END) != 0)
    {
        jzWarning() << "JZFile::size failed: fseek error";
        return 0;
    }

    long sz = ftell(m_fp);
    fseek(m_fp, cur, SEEK_SET);
    return (int)sz;
#else
    auto *inst = JZPlatform::instance();
    long cur = inst->ftell(m_fp);
    if (cur == -1)
    {
        jzWarning() << "JZFile::size failed: ftell error";
        return 0;
    }
    if (inst->fseek(m_fp, 0, SEEK_END) != 0)
    {
        jzWarning() << "JZFile::size failed: fseek error";
        return 0;
    }
    long sz = inst->ftell(m_fp);
    inst->fseek(m_fp, cur, SEEK_SET);
    return (int)sz;
#endif
}

bool JZFile::atEnd() const
{
    if (m_res)
        return m_resOffset >= m_res->size;
    if (!m_fp)
    {
        jzWarning() << "JZFile::atEnd: file not open, treat as end";
        return true;
    }
#ifdef JZ_OS_NORMAL
    return feof(m_fp);
#else
    return JZPlatform::instance()->feof(m_fp);
#endif
}

void JZFile::seek(int pos)
{
    if (m_res)
    {
        m_resOffset = pos;
        if (m_resOffset < 0) 
            m_resOffset = 0;
        if (m_resOffset > m_res->size) 
            m_resOffset = m_res->size;
        return;
    }
#ifdef JZ_OS_NORMAL
    fseek(m_fp, pos, SEEK_SET);
#else
    JZPlatform::instance()->fseek(m_fp, pos, SEEK_SET);
#endif
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
#ifdef JZ_OS_NORMAL
    fseek(m_fp, pos, origin);
#else
    JZPlatform::instance()->fseek(m_fp, pos, origin);
#endif
}

int JZFile::tell()
{
    if (m_res)
        return m_resOffset;
#ifdef JZ_OS_NORMAL
    return ftell(m_fp);
#else
    return (int)JZPlatform::instance()->ftell(m_fp);
#endif
}

int JZFile::write(const JZByteArray &buffer)
{
    return write(buffer.data(), buffer.size());
}

int JZFile::write(const char *buffer, int size)
{
    if (m_res)
    {
        jzWarning() << "JZFile::write failed: resource is read-only";
        return 0;
    }

#ifdef JZ_OS_NORMAL
    int ret = (int)fwrite(buffer, 1, size, m_fp);
    return ret;
#else
    int ret = (int)JZPlatform::instance()->fwrite(buffer, size, m_fp);
    return ret;
#endif
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
#ifdef JZ_OS_NORMAL
    return (int)fread(buffer, 1, size, m_fp);
#else
    return (int)JZPlatform::instance()->fread(buffer, size, m_fp);
#endif
}

JZByteArray JZFile::readAll()
{
    JZByteArray ret;
    while (true)
    {
        JZByteArray tmp = read(4 * 1024);
        if (tmp.isEmpty())
            break;

        ret.append(tmp);
    }
    return ret;
}
