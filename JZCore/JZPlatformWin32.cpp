
#include <Windows.h>
#include <string>
#include "JZStd.h"

std::string win32_applicationFilePath()
{
    wchar_t buffer[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, buffer, MAX_PATH);
    return wstr_to_str(std::wstring(buffer));
}

std::string win32_applicationDirPath()
{
    wchar_t buffer[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, buffer, MAX_PATH);
    std::string path = wstr_to_str(std::wstring(buffer));

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