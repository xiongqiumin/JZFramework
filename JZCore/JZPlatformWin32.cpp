
#include <Windows.h>
#include <string>
#include "JZStd.h"

std::string win32_applicationFilePath()
{
    char buffer[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::string path = str_local_to_utf8(buffer);
    return path;
}

std::string win32_applicationDirPath()
{
    char buffer[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::string path = str_local_to_utf8(buffer);

    size_t pos = path.find_last_of("\\/");
    if (pos != std::wstring::npos)
    {
        path = path.substr(0, pos);
    }
    str_replace(path, "\\", "/");
    return path;
}

std::string win32_applicationTmpPath()
{
    return "C:/tmp";
}