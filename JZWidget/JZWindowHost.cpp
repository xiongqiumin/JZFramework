// ==================== 窗口宿主 (线程局部开关, 见 JZWindowHost.h) ====================
#include "JZWindowHost.h"

namespace {

// **按线程**存: 窗口总是被"创建它的那个线程"托管 —— 一个进程一个线程跑一份 JS 时
// (见 binary/virtual_device 的 JZVirtualProcess), 每个进程各自装自己的宿主, 互不打扰。
// 不用全局变量: 那会让两个进程的引擎互相顶掉对方的宿主。
thread_local JZWindowHost* t_windowHost = nullptr;

} // namespace

void jzSetWindowHost(JZWindowHost* host)
{
    t_windowHost = host;
}

JZWindowHost* jzWindowHost()
{
    return t_windowHost;
}
