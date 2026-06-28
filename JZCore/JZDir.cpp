#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <regex>
#include "JZDir.h"
#include "JZLog.h"
#include "JZFileInfo.h"
#include "JZDebug.h"
#include "JZString.h"

using namespace std;

bool JZDir::isAbsolutePath(string path)
{
#ifdef _WIN32    
    if (path.size() >= 3 && isalpha(path[0]) && path[1] == ':') {
        char sep = path[2];
        if (sep == '\\' || sep == '/')
            return true;
    }
#else
    if(path.size() >= 1 && path[0] == '/')
        return true;
#endif

    return false;
}


string JZDir::currentPath()
{
    // 跨平台获取当前工作目录
#ifdef _WIN32
    char buffer[512];
    DWORD result = GetCurrentDirectoryA(512, buffer);
    return str_local_to_utf8(buffer);

#elif defined(__linux__)
    char* buffer = getcwd(nullptr, 0);
    if (buffer == nullptr)
        return string();

    string currentDir(buffer);
    free(buffer);
    return currentDir;
#else
    return string();
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
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(str_utf8_to_local(path).c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
#elif defined(__linux__)
    struct stat st;
    if(stat(path.c_str(), &st) != 0)
        return false;

    return true;
#else
    return false;    
#endif
}

bool JZDir::createDir(const string &path) const
{
#ifdef _WIN32
    string local_path = str_utf8_to_local(path);
    return CreateDirectoryA(local_path.c_str(), NULL) != 0;
#elif defined(__linux__)
    int ret = ::mkdir(path.c_str(), 0755);
    return (ret == 0);
#else
    return false;    
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

#ifdef _WIN32
    string local_path = str_utf8_to_local(dir_path);
    BOOL result = RemoveDirectoryA(local_path.c_str());
    return result == TRUE;
#elif defined(__linux__)
    int result = ::rmdir(dir_path.c_str());
    return (result == 0);
#else
    return false;
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
    if (JZDir::isAbsolutePath(m_path))
        return m_path;
    else
    {
        return currentPath() + "/" + m_path;
    }
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

vector<JZFileInfo> JZDir::entryInfoList(int filter, int sort) const {
    vector<JZFileInfo> result;

#ifdef _WIN32
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
    
#elif defined(__linux__) || defined(__ANDROID__)
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

    // 大小写转换 lambda
    auto toLower = [caseSensitive](JZChar c) -> JZChar
    {
        if (caseSensitive)
            return c;
        return c.toLower();
    };

    while (s < e)
    {
        JZChar sc = toLower(*s);
        JZChar pc = toLower(*p);

        if (pc == JZChar('*'))
        {
            // 记录 * 的位置：pStar 指向 * 下一个字符，sStar 记录当前字符串位置
            pStar = ++p;
            sStar = s;
        }
        else if (pc == JZChar('?') || pc == sc)
        {
            // 单个字符匹配成功，同时后移
            p++;
            s++;
        }
        else if (pStar != nullptr)
        {
            // 【修复核心】匹配失败，说明 * 需要多匹配一个字符
            s = ++sStar;  // 字符串后移一位
            p = pStar;    // 模式串回到 * 后面的位置重新匹配
        }
        else
        {
            // 无 * 可用，匹配失败
            return false;
        }
    }

    // 跳过模式串末尾剩余的所有 *
    while (*p == JZChar('*'))
    {
        p++;
    }

    // 模式串必须全部匹配完成才算成功
    return *p == JZChar('\0');
}

vector<JZFileInfo> JZDir::entryInfoList(const vector<string> &filters,int filter, int sort) const
{
    // 预转换所有过滤器为 JZString
    vector<JZString> jzFilters;
    jzFilters.reserve(filters.size());
    for (const auto &pattern : filters)
    {
        if (!pattern.empty())
        {
            jzFilters.push_back(JZString::fromStdString(pattern));
        }
    }

    if (jzFilters.empty())
    {
        return entryInfoList(filter, sort);
    }

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