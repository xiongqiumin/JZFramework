#include "JZFileDriverEsp32.h"
#include <dirent.h>
#include <unistd.h>

JZFileDriverEsp32::JZFileDriverEsp32() {}
JZFileDriverEsp32::~JZFileDriverEsp32() {}

bool JZFileDriverEsp32::isAbsolutePath(const std::string &path)
{
    return !path.empty() && path[0] == '/';
}

std::string JZFileDriverEsp32::currentPath()
{
    char *buf = getcwd(nullptr, 0);
    if (!buf) return {};
    std::string ret(buf);
    free(buf);
    return ret;
}

int JZFileDriverEsp32::stat(const std::string &path, struct stat *st)
{
    return ::stat(path.c_str(), st);
}

bool JZFileDriverEsp32::fileRemove(const std::string &path)
{
    return unlink(path.c_str()) == 0;
}

bool JZFileDriverEsp32::dirCreate(const std::string &path)
{
    return mkdir(path.c_str(), 0755) == 0;
}

bool JZFileDriverEsp32::dirRemove(const std::string &path)
{
    return rmdir(path.c_str()) == 0;
}

std::vector<std::string> JZFileDriverEsp32::dirEntryList(const std::string &path)
{
    std::vector<std::string> result;
    DIR *dir = opendir(path.c_str());
    if (!dir) return result;

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr)
        result.push_back(entry->d_name);

    closedir(dir);
    return result;
}
