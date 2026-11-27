// 崩溃报告自检(故意触发 0xC0000005, 不进 ctest)
//
// 用途: 验证 JZCrashReport 在 Windows 上能拿到符号化堆栈并落 <exe>/crash/*.log|.dmp。
// 期望: stderr 打出 [crash] code=0xC0000005 ... 以及带函数名/文件行的栈;
//       进程目录下生成 crash_<pid>.log 与 crash_<pid>.dmp。

#include <stdio.h>

#include "JZApplication.h"
#include "JZCrashReport.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    JZApplication app;

    JZCrashReportManager::instance()->init([](JZCrashReport report) {
        fprintf(stderr, "[crash] %s\n%s\n", report.msg.c_str(), report.stack.c_str());
        fflush(stderr);
    });

    fprintf(stderr, "about to crash...\n");
    fflush(stderr);

    // 故意写非法地址
    volatile int *bad = (int *)0x10;
    *bad = 1;

    return 0;
}
