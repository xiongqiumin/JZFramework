#include "JZWebView2.h"
#include "JZWebServer.h"
#include "JZApplication.h"

JZWebView2::JZWebView2() : JZObject() {}
JZWebView2::~JZWebView2() {}

void JZWebView2PostEvent(JZObject* view, int type)
{
    if (jzApp && view)
        jzApp->postEvent(view, new JZWebView2Event(type));
}

void JZWebView2::show(std::string url)
{
    if (url.find("http") != 0)
        url = JZWebServer::baseUrl() + "/" + url;
    JZWebView2Backend::instance()->createView(this, url);
}

void JZWebView2::hide()
{
    JZWebView2Backend::instance()->hideView(this);
}

void JZWebView2::navigate(std::string url)
{
    if (url.find("http") != 0)
        url = JZWebServer::baseUrl() + "/" + url;
    JZWebView2Backend::instance()->navigateView(this, url);
}

void JZWebView2::close()
{
    JZWebView2Backend::instance()->closeView(this);
}

void JZWebView2::customEvent(JZEvent* e)
{
    if (e->type() == JZWebView2Event_Closed)
    {
        // 页面已关闭（用户关闭窗口或程序调用 close()）。
        // 可在此更新 UI / 释放资源 / 发出信号。
        return;
    }
}
