#pragma once
#include <windows.h>
#include "JZObject.h"
#include <string>
#include <map>
#include <thread>

// JZWebView2Backend —— 单例。
//
// 内部持有 WebView2 所需的 STA 消息泵线程。原因：WebView2 必须运行在带 Win32 消息泵
// 的 STA 线程上，而 JZApplication::exec() 并不泵送 Win32 消息（它用 condition_variable
// 等待事件），因此这里单独维护一个线程。
//
// JZWebView2 直接调用下面的成员函数；命令通过 PostMessage 派发到 STA 线程的命令窗口，
// 由同一个 GetMessage 循环处理（WebView2 窗口消息与命令消息共用一条消息泵）。
class JZWebView2Backend
{
public:
    static JZWebView2Backend* instance();

    void createView(JZObject* view, const std::string& url);
    void hideView(JZObject* view);
    void navigateView(JZObject* view, const std::string& url);
    void closeView(JZObject* view);

private:
    JZWebView2Backend();
    ~JZWebView2Backend();

    void run();
    static LRESULT CALLBACK PageWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK CmdWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    std::thread m_thread;
    HWND m_cmdHwnd = nullptr;          // STA 线程上的命令窗口（消息only），接收命令
    std::map<JZObject*, HWND> m_views; // 仅 STA 线程访问
    int m_pageCount = 0;

    bool m_running = false;
    bool m_ownCom = false;
    bool m_classReg = false;
};

// 命令窗口的自定义消息
#define WM_WV_CREATE      (WM_USER + 100)
#define WM_WV_HIDE        (WM_USER + 101)
#define WM_WV_NAVIGATE    (WM_USER + 102)
#define WM_WV_CLOSE       (WM_USER + 103)
#define WM_WV_PAGE_CLOSED (WM_USER + 104)  // 页面窗口被销毁（用户关闭或程序 close）
#define WM_WV_QUIT        (WM_USER + 105)
