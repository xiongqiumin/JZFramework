#include "JZWebView2Backend.h"
#include "JZWebView2.h"
#include "JZApplication.h"
#include "WebView2.h"

#include <windows.h>

// ---------------------------------------------------------------------------
// 数据结构（仅 STA 线程访问）
// ---------------------------------------------------------------------------
struct PageData
{
    JZObject* view = nullptr;
    ICoreWebView2Controller* ctrl = nullptr;
};

// CreateWindowEx 时通过 lpCreateParams 传给 PageWndProc 的临时参数
struct CreateParam
{
    JZObject* view;
    std::wstring url;
};

// PostMessage 派发给命令窗口时携带的参数（堆分配，由 CmdWndProc 释放）
struct Req
{
    JZObject* view;
    std::wstring url;
};

// ---------------------------------------------------------------------------
// UTF-8 -> UTF-16
// ---------------------------------------------------------------------------
static std::wstring ToWstr(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w;
    w.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

// ---------------------------------------------------------------------------
// WebView2 COM 回调（裸 COM，手动实现 IUnknown）
// ---------------------------------------------------------------------------
class ControllerCompletedHandler
    : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
{
    ULONG m_ref = 1;
    HWND m_hwnd;
    std::wstring m_url;
public:
    ControllerCompletedHandler(HWND hwnd, const std::wstring& url) : m_hwnd(hwnd), m_url(url) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        if (--m_ref == 0) delete this;
        return m_ref;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown ||
            riid == IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)
        {
            *ppv = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT res, ICoreWebView2Controller* ctrl) override
    {
        if (SUCCEEDED(res) && ctrl)
        {
            ctrl->AddRef();
            auto* pd = reinterpret_cast<PageData*>(GetWindowLongPtr(m_hwnd, GWLP_USERDATA));
            if (pd)
                pd->ctrl = ctrl;
            else
                ctrl->Release();

            RECT rc;
            GetClientRect(m_hwnd, &rc);
            ctrl->put_Bounds(rc);

            ICoreWebView2* wv = nullptr;
            ctrl->get_CoreWebView2(&wv);
            if (wv)
            {
                if (!m_url.empty()) wv->Navigate(m_url.c_str());
                wv->Release();
            }
        }
        Release();
        return S_OK;
    }
};

class EnvCompletedHandler
    : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
{
    ULONG m_ref = 1;
    HWND m_hwnd;
    std::wstring m_url;
public:
    EnvCompletedHandler(HWND hwnd, const std::wstring& url) : m_hwnd(hwnd), m_url(url) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        if (--m_ref == 0) delete this;
        return m_ref;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown ||
            riid == IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)
        {
            *ppv = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT res, ICoreWebView2Environment* env) override
    {
        if (SUCCEEDED(res) && env)
        {
            auto* h = new ControllerCompletedHandler(m_hwnd, m_url);
            env->CreateCoreWebView2Controller(m_hwnd, h);
        }
        Release();
        return S_OK;
    }
};

// ---------------------------------------------------------------------------
// 页面窗口
// ---------------------------------------------------------------------------
static const wchar_t* kPageClass = L"JZWebView2Page";
static const wchar_t* kCmdClass  = L"JZWebView2Cmd";

static LRESULT CALLBACK PageWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        auto* cp = reinterpret_cast<CreateParam*>(
            reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);
        auto* pd = new PageData{ cp->view, nullptr };
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pd));

        auto* envH = new EnvCompletedHandler(hwnd, cp->url);
        CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr, envH);
        delete cp;
        return 0;
    }
    case WM_SIZE:
    {
        auto* pd = reinterpret_cast<PageData*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (pd && pd->ctrl)
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            pd->ctrl->put_Bounds(rc);
        }
        return 0;
    }
    case WM_DESTROY:
    {
        auto* pd = reinterpret_cast<PageData*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (pd)
        {
            if (pd->ctrl) pd->ctrl->Release();
            JZObject* view = pd->view;
            HWND cmdHwnd = JZWebView2Backend::instance()->m_cmdHwnd;
            delete pd;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);

            // 1) 通知 JZWebView2 对象（在主线程 customEvent 中处理）
            JZWebView2PostEvent(view, JZWebView2Event_Closed);
            // 2) 通知 backend 命令窗口：页面已销毁，由其处理 map/计数/退出
            PostMessage(cmdHwnd, WM_WV_PAGE_CLOSED, reinterpret_cast<WPARAM>(view), 0);
        }
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 命令窗口（运行在 STA 线程，接收 JZWebView2 发来的命令）
// ---------------------------------------------------------------------------
LRESULT CALLBACK JZWebView2Backend::CmdWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* b = JZWebView2Backend::instance();
    switch (msg)
    {
    case WM_WV_CREATE:
    {
        auto* r = reinterpret_cast<Req*>(lParam);
        if (b->m_classReg)
        {
            HWND page = CreateWindowEx(
                0, kPageClass, L"JZ WebView2", WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700,
                nullptr, nullptr, GetModuleHandle(nullptr),
                new CreateParam{ r->view, r->url });
            if (page)
            {
                b->m_views[r->view] = page;
                b->m_pageCount++;
                ShowWindow(page, SW_SHOWNORMAL);
            }
            else
            {
                JZWebView2PostEvent(r->view, JZWebView2Event_Closed);
            }
        }
        delete r;
        return 0;
    }
    case WM_WV_NAVIGATE:
    {
        auto* r = reinterpret_cast<Req*>(lParam);
        auto it = b->m_views.find(r->view);
        if (it != b->m_views.end())
        {
            auto* pd = reinterpret_cast<PageData*>(GetWindowLongPtr(it->second, GWLP_USERDATA));
            if (pd && pd->ctrl)
            {
                ICoreWebView2* wv = nullptr;
                pd->ctrl->get_CoreWebView2(&wv);
                if (wv)
                {
                    wv->Navigate(r->url.c_str());
                    wv->Release();
                }
            }
        }
        delete r;
        return 0;
    }
    case WM_WV_HIDE:
    {
        auto it = b->m_views.find(reinterpret_cast<JZObject*>(wParam));
        if (it != b->m_views.end()) ShowWindow(it->second, SW_HIDE);
        return 0;
    }
    case WM_WV_CLOSE:
    {
        auto it = b->m_views.find(reinterpret_cast<JZObject*>(wParam));
        if (it != b->m_views.end()) DestroyWindow(it->second);
        return 0;
    }
    case WM_WV_PAGE_CLOSED:
    {
        // 页面窗口已销毁：更新 backend 状态（仅 STA 线程访问 m_views/m_pageCount）
        auto* view = reinterpret_cast<JZObject*>(wParam);
        b->m_views.erase(view);
        b->m_pageCount--;
        if (b->m_pageCount <= 0)
        {
            // 最后一个页面关闭：退出整个应用
            if (jzApp) jzApp->quit();
        }
        return 0;
    }
    case WM_WV_QUIT:
    {
        auto pages = b->m_views;
        for (auto& kv : pages) DestroyWindow(kv.second);
        b->m_views.clear();
        b->m_pageCount = 0;
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 单例
// ---------------------------------------------------------------------------
JZWebView2Backend* JZWebView2Backend::instance()
{
    static JZWebView2Backend s_inst;
    return &s_inst;
}

JZWebView2Backend::JZWebView2Backend()
{
    // 启动 WebView2 所需的 STA 消息泵线程
    m_running = true;
    m_thread = std::thread(&JZWebView2Backend::run, this);
}

JZWebView2Backend::~JZWebView2Backend()
{
    // 停止 STA 线程：销毁剩余页面并退出消息循环，再 join
    if (m_cmdHwnd) PostMessage(m_cmdHwnd, WM_WV_QUIT, 0, 0);
    if (m_thread.joinable()) m_thread.join();
}

void JZWebView2Backend::createView(JZObject* view, const std::string& url)
{
    auto* r = new Req{ view, ToWstr(url) };
    PostMessage(m_cmdHwnd, WM_WV_CREATE, 0, reinterpret_cast<LPARAM>(r));
}

void JZWebView2Backend::hideView(JZObject* view)
{
    PostMessage(m_cmdHwnd, WM_WV_HIDE, reinterpret_cast<WPARAM>(view), 0);
}

void JZWebView2Backend::navigateView(JZObject* view, const std::string& url)
{
    auto* r = new Req{ view, ToWstr(url) };
    PostMessage(m_cmdHwnd, WM_WV_NAVIGATE, 0, reinterpret_cast<LPARAM>(r));
}

void JZWebView2Backend::closeView(JZObject* view)
{
    PostMessage(m_cmdHwnd, WM_WV_CLOSE, reinterpret_cast<WPARAM>(view), 0);
}

// ---------------------------------------------------------------------------
// STA 消息泵线程
// ---------------------------------------------------------------------------
void JZWebView2Backend::run()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_ownCom = (hr == S_OK);

    WNDCLASSEX pc = { sizeof(pc) };
    pc.lpfnWndProc = PageWndProc;
    pc.lpszClassName = kPageClass;
    pc.hInstance = GetModuleHandle(nullptr);
    m_classReg = RegisterClassEx(&pc) != 0;

    WNDCLASSEX cc = { sizeof(cc) };
    cc.lpfnWndProc = CmdWndProc;
    cc.lpszClassName = kCmdClass;
    cc.hInstance = GetModuleHandle(nullptr);
    RegisterClassEx(&cc);

    // 消息only窗口，专门接收命令
    m_cmdHwnd = CreateWindowEx(
        0, kCmdClass, nullptr, 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, nullptr, nullptr);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (m_cmdHwnd) DestroyWindow(m_cmdHwnd);
    m_cmdHwnd = nullptr;
    if (m_classReg)
    {
        UnregisterClass(kPageClass, GetModuleHandle(nullptr));
        m_classReg = false;
    }
    UnregisterClass(kCmdClass, GetModuleHandle(nullptr));

    if (m_ownCom) CoUninitialize();
}
