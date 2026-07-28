#pragma once
#include "JZWebView2Backend.h"
#include "JZEvent.h"

// 自定义事件类型（>= JZEvent_user，由 backend 经 jzApp->postEvent 回送）
const int JZWebView2Event_Closed = JZEvent_user + 1;

class JZWebView2Event : public JZEvent
{
public:
    explicit JZWebView2Event(int type) : JZEvent(type) {}
};

// backend 用来把事件回送给 JZWebView2 对象
void JZWebView2PostEvent(JZObject* view, int type);

// JZWebView2 —— 代表一个浏览器窗口。
// show/hide/navigate/close 会向 JZWebView2Backend 发送相应消息；
// 页面关闭时 backend 通过 jzApp->postEvent 回送 JZWebView2Event_Closed。
class JZWebView2 : public JZObject
{
public:
    JZWebView2();
    ~JZWebView2();

    void show(std::string url);     // 打开页面（相对路径会拼成 http 服务地址）
    void hide();                    // 隐藏页面
    void navigate(std::string url); // 导航到新地址
    void close();                   // 关闭页面

protected:
    void customEvent(JZEvent* e) override;
};
