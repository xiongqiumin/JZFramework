#include "JZCrashReport.h"
#include <execinfo.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <string>
#include <cstring>
#include <unistd.h>   // 提供 getpid()
#include <signal.h>   // 提供 kill()
#include "JZLog.h"

using namespace std;

// 内部实现类
class JZCrashReportImpl
{
public:
    JZCrashCallback callback = nullptr;

    string getStackTrace()
    {
        const int MAX_STACK = 30;
        void* buffer[MAX_STACK];
        int n = backtrace(buffer, MAX_STACK);
        char** syms = backtrace_symbols(buffer, n);

        string stack;
        for (int i = 0; i < n; ++i) {
            stack += syms[i];
            stack += "\n";
        }
        free(syms);
        return stack;
    }
};

// 信号处理函数
void JZCrashReportSignalHandler(int sig)
{
    auto* impl = JZCrashReportManager::instance()->d;

    char buf[64];
    snprintf(buf, sizeof(buf), "程序崩溃，信号：%d", sig);

    JZCrashReport report;
    report.platform = "linux";
    report.assert = JZLogManager::instance()->fatalError();
    report.signal = sig;
    report.msg = buf;
    report.stack = impl->getStackTrace();

    impl->callback(report);
    
    // 2. 恢复信号的默认处理方式
    struct sigaction default_action;
    sigemptyset(&default_action.sa_mask);
    default_action.sa_handler = SIG_DFL;  // 恢复默认处理
    default_action.sa_flags = 0;
    sigaction(sig, &default_action, nullptr);

    // 3. 重新发送该信号，让系统执行默认行为（如终止进程）
    kill(getpid(), sig);
}

// 管理类构造
JZCrashReportManager::JZCrashReportManager() {
    d = new JZCrashReportImpl();
}

JZCrashReportManager::~JZCrashReportManager() {
    delete d;
}

JZCrashReportManager* JZCrashReportManager::instance() {
    static JZCrashReportManager inst;
    return &inst;
}

void JZCrashReportManager::init(JZCrashCallback callback) {
    d->callback = callback;

    // 使用 sigaction 替换 signal，更稳定、更标准
    struct sigaction sa;
    sa.sa_handler = JZCrashReportSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_SIGINFO;

    sigaction(SIGSEGV, &sa, nullptr);  // 段错误
    sigaction(SIGABRT, &sa, nullptr);  // 断言/abort
    sigaction(SIGFPE,  &sa, nullptr);  // 除零
    sigaction(SIGILL,  &sa, nullptr);  // 非法指令
    sigaction(SIGBUS,  &sa, nullptr);  // 总线错误
}

JZOptional<JZCrashReport> JZCrashReportManager::takePendingReport()
{
    return JZOptional<JZCrashReport>();  // Linux 平台无复位持久化崩溃记录
}