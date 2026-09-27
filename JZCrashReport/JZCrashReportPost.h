#ifndef JZ_CRASH_REPORT_POST_H
#define JZ_CRASH_REPORT_POST_H
#include "JZObject.h"
#include "JZCrashReport.h"

// 崩溃报告异步上报器。
// 通过 JZCrashReportPost::post(url, report) 把一份 JZCrashReport 序列化后,
// 在独立线程里 POST 到指定 HTTP 地址; 发送完成(成功/失败/超时)后该线程自动退出。
// 上报器实例与发送线程都挂在 jzApp 下, 随应用生命周期管理。
class JZFRAMEWORK_API JZCrashReportPost : public JZObject
{
public:
    // 异步上报。非阻塞: 立即返回, 实际发送在工作线程中进行。
    static void post(const std::string &url, const JZCrashReport &report);

protected:    
};

#endif // !JZ_CRASH_REPORT_POST_H
