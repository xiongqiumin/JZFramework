#include "JZDir.h"
#include "JZLog.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZResources.h"
#include "JZDebug.h"
#include "JZString.h"
#include "JZPlatform.h"

#if JZ_OS == JZ_OS_WINDOWS
#include <windows.h>
#elif JZ_OS == JZ_OS_LINUX
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace std;

bool JZDir::isAbsolutePath(string path)
{
    // 资源路径以 ':/' 开头, 视为绝对路径 (与 JZFile/JZFileInfo 一致)
    if (JZFileInfo::isResourcePath(path))
        return true;

#if JZ_OS == JZ_OS_WINDOWS
    if (path.size() >= 2 && isalpha(path[0]) && path[1] == ':') {
        return true;
    }
    if (path.size() >= 1 && path[0] == '/')
        return true;

    return false;
#else
    return path.size() >= 1 && path[0] == '/';
#endif
}


// copyRecursively 的递归体：不清空 *error，保证最内层的原因能传到调用方
static bool copyDirRecursiveImpl(const string &src, const string &dst, bool force, string *error)
{
    JZFileInfo srcInfo(src);
    if (!srcInfo.isDir())
    {
        if (error) *error = "源不是目录: " + src;
        return false;
    }

    JZDir dstDir(dst);
    if (!dstDir.mkpath(dst))
    {
        if (error) *error = "无法创建目录: " + dst;
        return false;
    }

    string dstPath = dst;
    while (dstPath.size() > 1 && (dstPath.back() == '/' || dstPath.back() == '\\'))
        dstPath.pop_back();

    JZDir srcDir(src);
    vector<JZFileInfo> infos =
        srcDir.entryInfoList(JZDir::Files | JZDir::Dirs | JZDir::NoDotAndDotDot);
    for (const auto &fi : infos)
    {
        string s = fi.filePath();
        string d = dstPath + "/" + fi.fileName();
        if (fi.isDir())
        {
            if (!copyDirRecursiveImpl(s, d, force, error))
                return false;
        }
        else
        {
            if (!JZFile::copy(s, d, force, error))
                return false;
        }
    }
    return true;
}

bool JZDir::copyRecursively(const string &srcPath, const string &dstPath,
                            bool force, string *error)
{
    if (error)
        error->clear();
    return copyDirRecursiveImpl(srcPath, dstPath, force, error);
}

bool JZDir::rename(const string &oldPath, const string &newPath, string *error)
{
    return JZFile::rename(oldPath, newPath, error);
}

// 设置进程当前工作目录(对应 Qt 的 QDir::setCurrent, 本框架改名 setCurrentPath 与 currentPath() 对称)。
// path 为 UTF-8。失败时返回 false 且不动当前目录; 其余平台(如 ESP32)没有进程工作目录概念, 直接 false。
bool JZDir::setCurrentPath(const string &path)
{
    if (path.empty())
        return false;

#if JZ_OS == JZ_OS_WINDOWS
    return SetCurrentDirectoryA(str_utf8_to_local(path).c_str()) != 0;
#elif JZ_OS == JZ_OS_LINUX
    return ::chdir(path.c_str()) == 0;
#else
    return false;
#endif
}

string JZDir::currentPath()
{
#if JZ_OS == JZ_OS_WINDOWS
    char buffer[512];
    DWORD result = GetCurrentDirectoryA(512, buffer);
    return str_local_to_utf8(buffer);
#elif JZ_OS == JZ_OS_LINUX
    char* buffer = getcwd(nullptr, 0);
    if (buffer == nullptr)
        return string();
    string currentDir(buffer);
    free(buffer);
    return currentDir;
#else
    return JZPlatform::instance()->currentPath();
#endif
}

JZDir::JZDir() 
{
}

JZDir::JZDir(const string &path)     
{
    setPath(path);
}

bool JZDir::dirExists(const string &path) const
{
#if JZ_OS == JZ_OS_WINDOWS
    DWORD attr = GetFileAttributesA(str_utf8_to_local(path).c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
#elif JZ_OS == JZ_OS_LINUX
    struct stat st;
    return stat(path.c_str(), &st) == 0;
#else
    auto st = JZPlatform::instance()->fileStat(path);
    return st.exists;
#endif
}

bool JZDir::createDir(const string &path) const
{
#if JZ_OS == JZ_OS_WINDOWS
    string local_path = str_utf8_to_local(path);
    return CreateDirectoryA(local_path.c_str(), NULL) != 0;
#elif JZ_OS == JZ_OS_LINUX
    return ::mkdir(path.c_str(), 0755) == 0;
#else
    return JZPlatform::instance()->dirCreate(path);
#endif
}

bool JZDir::removeDirRev(const string &path) const
{
    JZDir dir(path);
    auto list = dir.entryInfoList(Files | Dirs | NoDotAndDotDot);
    for (int i = 0; i < (int)list.size(); i++)
    {
        if (list[i].isDir())
        {
            if (!removeDirRev(list[i].filePath()))
                return false;
        }
        else
        {
            if (!JZFile::remove(list[i].filePath()))
                return false;
        }
    }
    return dir.rmdir(".");
}

string JZDir::name() const
{
    JZFileInfo info(m_path);
    return info.fileName();
}

bool JZDir::exists() const
{
    if (isResource())
    {
        // 资源组存在 == 该组下至少注册了一个资源
        std::string group_prefix = m_path;
        if (group_prefix.back() != '/')
            group_prefix += '/';

        auto all = JZResources::instance()->resourcesList();
        for (const auto &name : all)
        {
            if (name.rfind(group_prefix, 0) == 0)
                return true;
        }
        return false;
    }
    return dirExists(m_path);
}

bool JZDir::exists(const string &path) const
{
    string dir_path;

    if (isAbsolutePath(path))
        dir_path = path;
    else
        dir_path = absolutePath() + "/" + path;

    dir_path = JZFileInfo::weaklyCanonicalPath(dir_path);
    return dirExists(dir_path);
}

bool JZDir::mkdir(const string &path)
{
    string dir_path = absoluteFilePath(path);

    dir_path = JZFileInfo::weaklyCanonicalPath(dir_path);
    return createDir(dir_path);
}

bool JZDir::rmdir(const string &path)
{
    string dir_path = absoluteFilePath(path);
    dir_path = JZFileInfo::weaklyCanonicalPath(dir_path);

#if JZ_OS == JZ_OS_WINDOWS
    string local_path = str_utf8_to_local(dir_path);
    return RemoveDirectoryA(local_path.c_str()) == TRUE;
#elif JZ_OS == JZ_OS_LINUX
    return ::rmdir(dir_path.c_str()) == 0;
#else
    return JZPlatform::instance()->dirRemove(dir_path);
#endif
}

bool JZDir::mkpath(const string &path)
{
    string dir_path = absoluteFilePath(path);

    // 2. 规范化路径（和 mkdir 保持一致）
    dir_path = JZFileInfo::weaklyCanonicalPath(dir_path);

    // 3. 逐级创建多级目录
    size_t pos = 0;
    while (pos < dir_path.size())
    {
        // 查找下一个路径分隔符 /
        pos = dir_path.find_first_of("/\\", pos + 1);
        if (pos == string::npos)
            pos = dir_path.size();   // 已到最后一段，处理末尾组件后结束循环

        // 截取到当前分隔符的子路径
        string sub_path = dir_path.substr(0, pos);

        // 空路径跳过（根路径 / ）
        if (sub_path.empty())
            continue;

        // 如果不存在，创建单层目录
        if (!exists(sub_path))
        {
            if (!createDir(sub_path))
                return false;
        }
    }

    return true;
}

bool JZDir::remove(const string &fileName)
{
    string full_name = absoluteFilePath(fileName);
    return JZFile::remove(full_name);
}

bool JZDir::removeRecursively()
{
    return removeDirRev(absolutePath());
}

void JZDir::setPath(const string &path) 
{
    m_path = path;
    str_replace(m_path, "\\", "/");
    while(str_contains(m_path, "//"))
    {
        str_replace(m_path, "//", "/");
    }
}

string JZDir::path() const 
{
    return m_path;
}

string JZDir::absolutePath() const
{
    if (isResource())
        return m_path;
    else if (JZDir::isAbsolutePath(m_path))
        return m_path;
    else
    {
        return currentPath() + "/" + m_path;
    }
}

// 切到上一级目录(同 Qt 的 QDir::cdUp())。见 JZDir.h: 已经在根上就返回 false、目录不动。
bool JZDir::cdUp()
{
    string p = m_path;
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    if (p.empty() || p == "/")
        return false;

    size_t pos = p.find_last_of('/');
    if (pos == string::npos)
        return false;                       // 无分隔符的相对路径 / 盘符根 "C:"
    if (pos == 0)
        setPath("/");                       // "/x" -> "/"
    else if (p[pos - 1] == ':')
        setPath(p.substr(0, pos + 1));      // "C:/a" -> "C:/"(盘符根保留尾斜杠)
    else
        setPath(p.substr(0, pos));
    return true;
}

string JZDir::absoluteFilePath(string path) const
{
    if (JZDir::isAbsolutePath(path))
        return path;
    else
    {
        if (path.empty())
            return absolutePath();
        else
            return absolutePath() + "/" + path;
    }
}

bool JZDir::isResource() const
{
    return JZFileInfo::isResourcePath(m_path);
}

vector<JZFileInfo> JZDir::entryInfoList(int filter, int sort) const {
    vector<JZFileInfo> result;

    // 资源目录: 路径以 ':/' 开头, 列出该资源组下的所有资源项
    if (JZFileInfo::isResourcePath(m_path))
    {
        std::string group_prefix = m_path;
        if (group_prefix.back() != '/')
            group_prefix += '/';

        auto all = JZResources::instance()->resourcesList();
        for (const auto &name : all)
        {
            if (name.rfind(group_prefix, 0) != 0)
                continue;

            // 资源组下的直接子项 (不含更深层级)
            std::string sub = name.substr(group_prefix.size());
            if (sub.empty())
                continue;
            if (sub.find('/') != std::string::npos)
                continue;

            JZFileInfo info(name);
            if ((filter & Files) && info.isFile())
                result.push_back(info);
            else if ((filter & Dirs) && info.isDir())
                result.push_back(info);
        }

        if (sort & SortName) {
            std::sort(result.begin(), result.end(), [](const JZFileInfo &a, const JZFileInfo &b) {
                return a.fileName() < b.fileName();
            });
        }
        else if (sort & SortSize) {
            std::sort(result.begin(), result.end(), [](const JZFileInfo &a, const JZFileInfo &b) {
                return a.size() < b.size();
            });
        }
        else if (sort & SortTime) {
            std::sort(result.begin(), result.end(), [](const JZFileInfo &a, const JZFileInfo &b) {
                return a.lastModified() < b.lastModified();
            });
        }
        if (sort & SortReversed)
            reverse(result.begin(), result.end());

        return result;
    }

#if JZ_OS == JZ_OS_WINDOWS
    string searchPath = str_utf8_to_local(m_path) + "/*";
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);

    if (hFind == INVALID_HANDLE_VALUE) 
        return result;

    do {
        string name = str_local_to_utf8(findData.cFileName);
        if ((filter & NoDotAndDotDot) && (name == "." || name == "..")) continue;

        string fullPath = m_path + "/" + name;
        JZFileInfo info(fullPath);

        if ((filter & Files) && info.isFile()) {
            result.push_back(info);
        }
        else if ((filter & Dirs) && info.isDir()) {
            result.push_back(info);
        }
    } while (FindNextFileA(hFind, &findData) != 0);

    FindClose(hFind);
    
#elif JZ_OS == JZ_OS_LINUX || defined(__ANDROID__)
    DIR *dir = opendir(m_path.c_str());
    if (!dir) return result;

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        string name = entry->d_name;
        if ((filter & NoDotAndDotDot) && (name == "." || name == "..")) continue;

        string fullPath = m_path + "/" + name;
        JZFileInfo info(fullPath);

        if ((filter & Files) && info.isFile()) {
            result.push_back(info);
        }
        else if ((filter & Dirs) && info.isDir()) {
            result.push_back(info);
        }
    }
    closedir(dir);
#else
    auto names = JZPlatform::instance()->dirEntryList(m_path);
    for (auto &name : names)
    {
        if ((filter & NoDotAndDotDot) && (name == "." || name == "..")) continue;

        string fullPath = m_path + "/" + name;
        JZFileInfo info(fullPath);

        if ((filter & Files) && info.isFile())
            result.push_back(info);
        else if ((filter & Dirs) && info.isDir())
            result.push_back(info);
    }
#endif

    if (sort & SortName) {
        std::sort(result.begin(), result.end(), [](const JZFileInfo &a, const JZFileInfo &b) {
            return a.fileName() < b.fileName();
        });
    }
    else if (sort & SortSize) {
        std::sort(result.begin(), result.end(), [](const JZFileInfo &a, const JZFileInfo &b) {
            return a.size() < b.size();
        });
    }
    else if (sort & SortTime) {
        std::sort(result.begin(), result.end(), [](const JZFileInfo &a, const JZFileInfo &b) {
            return a.lastModified() < b.lastModified();
        });
    }

    if (sort & SortReversed) {
        reverse(result.begin(), result.end());
    }

    return result;
}

static bool matchUnicodeWildcard(const JZString &str, const JZString &pattern, bool caseSensitive = false)
{
    const JZChar *s = str.data();
    const JZChar *e = str.data() + str.size();
    const JZChar *p = pattern.data();
    const JZChar *sStar = nullptr;
    const JZChar *pStar = nullptr;

    auto toLower = [caseSensitive](JZChar c) -> JZChar
    {
        if (caseSensitive) return c;
        return c.toLower();
    };

    while (s < e)
    {
        JZChar sc = toLower(*s);
        JZChar pc = toLower(*p);

        if (pc == JZChar('*'))
        {
            pStar = ++p;
            sStar = s;
        }
        else if (pc == JZChar('?') || pc == sc)
        {
            p++;
            s++;
        }
        else if (pStar != nullptr)
        {
            s = ++sStar;
            p = pStar;
        }
        else
        {
            return false;
        }
    }

    while (*p == JZChar('*')) p++;
    return *p == JZChar('\0');
}

vector<JZFileInfo> JZDir::entryInfoList(const vector<string> &filters, int filter, int sort) const
{
    vector<JZString> jzFilters;
    jzFilters.reserve(filters.size());
    for (const auto &pattern : filters)
    {
        if (!pattern.empty())
            jzFilters.push_back(JZString::fromStdString(pattern));
    }

    if (jzFilters.empty())
        return entryInfoList(filter, sort);

    auto list = entryInfoList(filter, sort);
    vector<JZFileInfo> result;
    result.reserve(list.size());

    for (const auto &info : list)
    {
        JZString fileName = JZString::fromStdString(info.fileName());
        for (const auto &pattern : jzFilters)
        {
            if (matchUnicodeWildcard(fileName, pattern))
            {
                result.push_back(info);
                break;
            }
        }
    }

    return result;
}

vector<string> JZDir::entryList(int filter, int sort) const 
{
    vector<string> names;
    auto infos = entryInfoList(filter, sort);
    for (const auto &info : infos) {
        names.push_back(info.fileName());
    }
    return names;
}

vector<string> JZDir::entryList(const vector<string> &filters, int filter, int sort) const 
{
    vector<string> names;
    auto infos = entryInfoList(filters, filter, sort);
    for (const auto &info : infos) {
        names.push_back(info.fileName());
    }
    return names;
}
