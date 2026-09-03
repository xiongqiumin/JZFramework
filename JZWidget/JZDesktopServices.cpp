#include "JZDesktopServices.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>   // ShellExecute
#else
#include <cstdlib>
#include <unistd.h>
#endif

bool JZDesktopServices::openUrl(const std::string& url)
{
    if (url.empty())
        return false;

#ifdef _WIN32
    // ShellExecute 用系统关联程序打开 (浏览器/文件管理器/对应 App),
    // "open" 动词 + 完整 URL 即可, 含客户区外的窗口激活交给系统。
    // 返回值为 HINSTANCE, <=32 表示失败 (见 ShellExecute 文档)。
    HINSTANCE r = ShellExecuteA(nullptr, "open", url.c_str(),
                                nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;

#else
    // 非 Windows: 借助 xdg-open / open 命令 (桌面环境通常有)。
    const char* cmd = "xdg-open";
#if defined(__APPLE__)
    cmd = "open";
#endif
    std::string full = std::string(cmd) + " \"" + url + "\"";
    int ret = system(full.c_str());
    return ret == 0;

#endif
}
