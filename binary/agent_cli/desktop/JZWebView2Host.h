#ifndef JZ_WEBVIEW2_HOST_H_
#define JZ_WEBVIEW2_HOST_H_

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

struct ICoreWebView2;
struct ICoreWebView2Controller;
struct ICoreWebView2Environment;

// ---------------------------------------------------------------------------
// 最小 WebView2 COM 宿主(2026-11-25)
//
// 只做一件事: 起一个 Win32 窗口, 用 ICoreWebView2Controller 把 WebView2 贴进去,
// 然后 Navigate 到给它的 URL。消息循环由 JZApplication::exec() 统一驱动。
// 没有 CEF, 没有 JZWidget, 也不接 host object。
//
// 真实 agent 页面仍应走本地 HTTP: SPA 用 fetch/EventSource 打 /api/*, file://
// 会 origin=null 撞 CORS。这里默认打开的 test_page.html 只是纯静态冒烟页。
// ---------------------------------------------------------------------------
class JZWebView2Host
{
public:
    ~JZWebView2Host();

    bool initialize(int argc, char **argv);
    bool createWindow(const std::string &url);
    void shutdown();

    bool isAvailable() const { return m_available; }
    const std::string &url() const { return m_url; }

    // 给 .cpp 里的 COM 回调用, 外部不要直接调。
    HRESULT onEnvironmentCreated(ICoreWebView2Environment *env);
    HRESULT onControllerCreated(ICoreWebView2Controller *controller);

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    std::wstring utf8ToWide(const std::string &text) const;
    std::wstring userDataDir();

    std::string m_url;
    std::wstring m_navigateUrl;
    std::wstring m_userDataDir;

    HWND m_hwnd = nullptr;
    bool m_available = false;
    bool m_comInitialized = false;

    ICoreWebView2Environment *m_environment = nullptr;
    ICoreWebView2Controller *m_controller = nullptr;
    ICoreWebView2 *m_webview = nullptr;
};

#endif // !JZ_WEBVIEW2_HOST_H_
