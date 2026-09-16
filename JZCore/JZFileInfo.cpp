#include <cstring>
#include <cstdio>
#include "JZStd.h"

#if JZ_OS == JZ_OS_WINDOWS
#include <io.h>       // _stat
#include <direct.h>   // _getcwd
#define stat _stat
#elif JZ_OS == JZ_OS_LINUX
#include <sys/stat.h> // POSIX stat
#endif

#include "JZFileInfo.h"
#include "JZDir.h"
#include "JZResources.h"
#include "JZPlatform.h"

bool JZFileInfo::exists(const std::string &path)
{
    return JZFileInfo(path).exists();
}

std::string JZFileInfo::weaklyCanonicalPath(std::string path)
{
    // 资源路径: 以 ':/' 开头, 保留 ':' 前缀, 仅规整其余部分
    if (JZFileInfo::isResourcePath(path))
    {
        std::string body = path;
        str_replace(body, "\\", "/");
        while (str_contains(body, "//"))
            str_replace(body, "//", "/");

        std::string rest = body.substr(2);   // 去掉 ":/"
        std::vector<std::string> processed;
        for (const auto &comp : str_split(rest, "/"))
        {
            if (comp.empty() || comp == ".")
                continue;
            else if (comp == "..")
            {
                if (!processed.empty())
                    processed.pop_back();
            }
            else
                processed.push_back(comp);
        }
        return ":/" + str_join(processed, "/");
    }

    assert(JZDir::isAbsolutePath(path));

    str_replace(path, "\\", "/");
    while (str_contains(path, "//"))
    {
        str_replace(path, "//", "/");
    }
  
    std::vector<std::string> processed;
    std::vector<std::string> components = str_split(path, "/");
    processed.push_back(components[0]);
    for (int i = 1; i < (int)components.size(); i++)
    {
        std::string comp = components[i];
        if (comp.empty() || comp == ".") {
            continue;
        }
        else if (comp == "..") {
            if (processed.size() > 1) {
                processed.pop_back();
            }
        }
        else {
            processed.push_back(comp);
        }
    }

    std::string result = str_join(processed, "/");
    return result;
}


JZFileInfo::JZFileInfo()
    : m_isResource(false), m_exists(false), m_isFile(false), m_isDir(false), m_size(0), m_time(0)
{
}

JZFileInfo::JZFileInfo(const std::string &path) 
{
    setFile(path);
}

void JZFileInfo::setFile(const std::string &path) 
{
    m_path = path;

    if (JZFileInfo::isResourcePath(path))
    {
        setResource(path);
        return;
    }

#if JZ_OS == JZ_OS_WINDOWS || JZ_OS == JZ_OS_LINUX
    struct stat st;
#if JZ_OS == JZ_OS_WINDOWS
    if (stat(str_utf8_to_local(path).c_str(), &st) == 0)
#else
    if (stat(path.c_str(), &st) == 0)
#endif
    {
        m_exists = true;

#if JZ_OS == JZ_OS_WINDOWS
        m_isFile = (st.st_mode & _S_IFREG) != 0;
        m_isDir = (st.st_mode & _S_IFDIR) != 0;
        m_time = static_cast<int64_t>(st.st_mtime) * 1000;
#else
        m_isFile = S_ISREG(st.st_mode);
        m_isDir = S_ISDIR(st.st_mode);
        m_time = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000 +
            static_cast<int64_t>(st.st_mtim.tv_nsec) / 1000000;
#endif

        m_size = st.st_size;
    }
    else {
        m_exists = false;
        m_isFile = false;
        m_isDir = false;
        m_size = 0;
        m_time = 0;
    }
#else
    JZFileStat stat = JZPlatform::instance()->fileStat(path);
    m_exists = stat.exists;
    m_isFile = stat.isFile;
    m_isDir  = stat.isDir;
    m_size   = stat.size;
    m_time   = stat.lastModified;
#endif
}

bool JZFileInfo::exists() const 
{
    return m_exists;
}

bool JZFileInfo::isFile() const 
{
    return m_isFile;
}

bool JZFileInfo::isDir() const
{
    return m_isDir;
}

bool JZFileInfo::isResource() const
{
    return m_isResource;
}

bool JZFileInfo::isResourcePath(const std::string &path)
{
    return path.rfind(":/", 0) == 0;
}

void JZFileInfo::setResource(const std::string &path)
{
    m_isResource = true;

    auto *res = JZResources::instance()->find(path);
    if (res != nullptr)
    {
        m_exists = true;
        m_isFile = true;
        m_isDir = false;
        m_size = res->size;
        m_time = res->mtime;
    }
    else
    {
        m_exists = false;
        m_isFile = false;
        m_isDir = false;
        m_size = 0;
        m_time = 0;
    }
}

std::string JZFileInfo::fileName() const 
{
    size_t pos = m_path.find_last_of("/\\");
    if (pos == std::string::npos) 
        return m_path;

    return m_path.substr(pos + 1);
}

std::string JZFileInfo::filePath() const 
{
    return m_path;
}

std::string JZFileInfo::path() const
{
    const auto& s = m_path;
    if (s.empty())
        return "";

    // 先判断末尾是否是分隔符：尾斜杠 → 直接返回完整字符串（Qt规则）
    char last = s.back();
    if (last == '/' || last == '\\')
    {
        return s;
    }

    size_t pos = s.find_last_of("/\\");
    if (pos == std::string::npos)
    {
        // Qt行为：纯文件名，无目录，返回 "."
        return ".";
    }

    // 根路径 "/"
    if (pos == 0)
    {
        return "/";
    }
#ifdef _WIN32
    // Windows盘符根 D:\
        if (pos == 2 && s.size() >= 2 && s[1] == ':')
    {
        return s.substr(0, pos + 1);
    }
#endif
    return s.substr(0, pos);
}

std::string JZFileInfo::baseName() const
{
    std::string full_name = fileName();
    size_t last_dot = full_name.find_last_of('.');

    std::string base_name;
    if (last_dot != std::string::npos) {
        base_name = full_name.substr(0, last_dot);
    }
    else {
        base_name = full_name;
    }
    return base_name;
}

std::string JZFileInfo::suffix() const
{
    std::string full_name = fileName();
    size_t last_dot = full_name.find_last_of('.');

    std::string suffix;
    if (last_dot != std::string::npos) {
        suffix = full_name.substr(last_dot + 1);
    }
    else {
        suffix = "";
    }
    return suffix;
}

int JZFileInfo::size() const 
{
    return m_size;
}

int64_t JZFileInfo::lastModified() const
{
    return m_time;
}
