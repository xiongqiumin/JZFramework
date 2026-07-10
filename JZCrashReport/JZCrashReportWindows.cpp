#include "Windows.h"
#include "JZCrashReport.h"

class JZCrashReportImpl
{
public:
    JZCrashCallback callback;
};

// Windows 异常捕获处理函数
static LONG WINAPI Win32ExceptionHandler(EXCEPTION_POINTERS* exceptionInfo)
{
    JZCrashReportSignalHandler(0);
    return EXCEPTION_EXECUTE_HANDLER;
}

void JZCrashReportSignalHandler(int sig)
{
    auto d = JZCrashReportManager::instance()->d;
    if (d->callback)
    {
        JZCrashReport rpt;
        rpt.platform = "windows";
        d->callback(rpt);
    }
}

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

void JZCrashReportManager::init(JZCrashCallback callback) 
{
    d->callback = callback;

    SetUnhandledExceptionFilter(Win32ExceptionHandler);
}

JZOptional<JZCrashReport> JZCrashReportManager::takePendingReport()
{
    return JZOptional<JZCrashReport>();  // Windows 平台无复位持久化崩溃记录
}