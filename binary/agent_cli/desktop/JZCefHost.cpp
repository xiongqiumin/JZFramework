#include "JZCefHost.h"

#include <stdio.h>

bool JZCefHost::initialize(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    fputs("[desktop] CEF stub: browser backend is not integrated yet\n", stderr);
    return true;
}

bool JZCefHost::createWindow(const std::string &url)
{
    m_url = url;
    fprintf(stderr, "[desktop] CEF stub: would open %s\n", url.c_str());
    return false;   // 没有真窗口, 只是占位
}

int JZCefHost::runMessageLoop()
{
    fputs("[desktop] CEF stub: no native window; open the same URL in a system browser\n", stderr);
    return 0;
}

void JZCefHost::shutdown()
{
    m_url.clear();
}
