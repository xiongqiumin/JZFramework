#include "JZCrashReportPost.h"

#include "JZCrashReport.h"
#include "JZApplication.h"
#include "JZThread.h"
#include "JZNetworkAccessManager.h"
#include "JZNetworkRequest.h"
#include "JZNetworkReply.h"
#include "JZByteArray.h" 
#include "JZDebug.h"

class ReportThread : public JZThread
{
public:
    std::string url;
    std::string body;

    ReportThread(JZObject *parent)
        : JZThread(parent)
    {
    }

    // 发送一次, 阻塞等待完成, 返回是否成功。
    bool doSend(JZNetworkAccessManager *mgr)
    {
        JZEventLoop loop;
        bool success = false;

        JZNetworkRequest req(url);
        req.setHeader(JZNetworkRequest::ContentTypeHeader, "text/plain");
        JZNetworkReply *reply = mgr->post(req, str_toUtf8(body));
        reply->setTimeOut(10 * 1000);

        connect(&reply->sigFinished, [&]() {
            success = (reply->error() == JZNetworkReply::NoError ||
                       reply->error() == JZNetworkReply::None);
            loop.quit();
        });

        loop.exec();
        return success;   // reply 为 mgr 子对象, mgr 离开作用域时一并回收
    }

    virtual void run()
    {
        JZNetworkAccessManager mgr;   // 局部变量, 仅创建一次, 析构时自动回收所有 reply
        const int maxRetries = 3;
        for (int attempt = 0; attempt < maxRetries; attempt++)
        {
            if (attempt > 0)
                JZThread::msleep(10 * 1000);   // 失败后等 10s 再重试

            if (doSend(&mgr))
                return;   // 成功 → 线程退出
        }
        // 失败 3 次 → 线程退出
        jzWarning() << "send crash report failed";
    }
};

void JZCrashReportPost::post(const std::string &url, const JZCrashReport &report)
{
    // 确保在 jzApp 下存在一个上报器实例(随应用生命周期管理)。
    ReportThread *t = new ReportThread(jzApp);
    t->url = url;
    t->body = JZCrashReportManager::instance()->makeReport(report);
    t->start();
}
