#ifndef JZ_CRASH_REPORT_H
#define JZ_CRASH_REPORT_H

#include <string>
#include "JZOptional.h"

struct JZCrashReport
{
    std::string assert; 
    std::string stack;   // 堆栈字符串
    int         signal;  // 崩溃信号
    std::string msg;     // 错误信息
};

typedef void (*JZCrashCallback)(JZCrashReport result);

class JZCrashReportImpl;

class JZCrashReportManager
{
public:
    static JZCrashReportManager* instance();

    void init(JZCrashCallback callback);
    std::string makeReport(const JZCrashReport &report);

private:
    friend void JZCrashReportSignalHandler(int sig);

    JZCrashReportManager();
    ~JZCrashReportManager();

    JZOptional<std::string> parseCrashReport(const std::string& crash_report);
    JZOptional<std::string> parseCrashReportLinux(const std::string& crash_report);

    std::pair<std::string, std::string> addr2line(const std::string& offset, const std::string& addr2line_path,
                                                  const std::string& exe_path);

    JZCrashReportImpl* d;
};

#endif