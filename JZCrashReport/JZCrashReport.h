#ifndef JZ_CRASH_REPORT_H
#define JZ_CRASH_REPORT_H
#include <string>
#include "JZOptional.h"

struct JZFRAMEWORK_API JZCrashReport
{
    std::string assert; 
    std::string stack;   // 堆栈字符串
    int         signal;  // 崩溃信号
    std::string msg;     // 错误信息
    std::string platform;// 平台标识: "esp32" / "linux" / "windows"
};

typedef void (*JZCrashCallback)(JZCrashReport result);

class JZCrashReportImpl;

class JZFRAMEWORK_API JZCrashReportManager
{
public:
    static JZCrashReportManager* instance();

    void init(JZCrashCallback callback);

    // 取回"上次复位前的崩溃报告"(若有)。ESP32 上崩溃信息在 panic 上下文写入
    // RTC_NOINIT 内存, 复位后由本函数在正常上下文中读取并清零, 供业务层上报。
    // 非 ESP32 平台(Windows/Linux)无持久化, 恒返回空。
    JZOptional<JZCrashReport> takePendingReport();

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