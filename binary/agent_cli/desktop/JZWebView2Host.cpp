#include "JZWebView2Host.h"

#define NOMINMAX
#include <WebView2.h>

#include <stdio.h>

namespace {

template <typename T>
class ComCallback : public T
{
public:
    explicit ComCallback(const IID &iid)
        : m_iid(iid)
        , m_ref(1)
    {
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, m_iid)) {
            *ppv = static_cast<T *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return ++m_ref; }

    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG ref = --m_ref;
        if (ref == 0)
            delete this;
        return ref;
    }

protected:
    virtual ~ComCallback() {}

private:
    IID m_iid;
    ULONG m_ref;
};

class EnvironmentCreatedHandler
    : public ComCallback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>
{
public:
    explicit EnvironmentCreatedHandler(JZWebView2Host *host)
        : ComCallback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
              IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)
        , m_host(host)
    {
    }

    STDMETHODIMP Invoke(HRESULT errorCode, ICoreWebView2Environment *result) override
    {
        if (FAILED(errorCode)) {
            fwprintf(stderr, L"[desktop] WebView2 environment failed: 0x%08lX\n",
                     static_cast<unsigned long>(errorCode));
        } else if (m_host) {
            m_host->onEnvironmentCreated(result);
        }
        Release();
        return S_OK;
    }

private:
    JZWebView2Host *m_host;
};

class ControllerCreatedHandler
    : public ComCallback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>
{
public:
    explicit ControllerCreatedHandler(JZWebView2Host *host)
        : ComCallback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
              IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)
        , m_host(host)
    {
    }

    STDMETHODIMP Invoke(HRESULT errorCode, ICoreWebView2Controller *result) override
    {
        if (FAILED(errorCode)) {
            fwprintf(stderr, L"[desktop] WebView2 controller failed: 0x%08lX\n",
                     static_cast<unsigned long>(errorCode));
        } else if (m_host) {
            m_host->onControllerCreated(result);
        }
        Release();
        return S_OK;
    }

private:
    JZWebView2Host *m_host;
};

class NavigationCompletedHandler
    : public ComCallback<ICoreWebView2NavigationCompletedEventHandler>
{
public:
    NavigationCompletedHandler()
        : ComCallback<ICoreWebView2NavigationCompletedEventHandler>(
              IID_ICoreWebView2NavigationCompletedEventHandler)
    {
    }

    STDMETHODIMP Invoke(ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) override
    {
        BOOL ok = FALSE;
        if (args && SUCCEEDED(args->get_IsSuccess(&ok))) {
            fputs(ok ? "[desktop] navigation completed: ok\n"
                     : "[desktop] navigation completed: failed\n",
                  stderr);
        }
        return S_OK;
    }
};

template <typename T>
void safeRelease(T *&p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

} // namespace

JZWebView2Host::~JZWebView2Host()
{
    shutdown();
}

bool JZWebView2Host::initialize(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
        fputs("[desktop] WebView2 needs STA; CoInitializeEx returned RPC_E_CHANGED_MODE\n", stderr);
        return false;
    }

    // S_FALSE 表示当前线程此前已经初始化过, 这里不负责配对的 CoUninitialize。
    m_comInitialized = (hr == S_OK);
    m_available = true;
    return true;
}

std::wstring JZWebView2Host::utf8ToWide(const std::string &text) const
{
    if (text.empty())
        return std::wstring();

    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (len <= 0)
        return std::wstring();

    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &out[0], len);
    return out;
}

std::wstring JZWebView2Host::userDataDir()
{
    if (!m_userDataDir.empty())
        return m_userDataDir;

    wchar_t localAppData[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? localAppData : L".";

    std::wstring dir = base + L"\\JZFramework";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\code_agent_webview2";
    CreateDirectoryW(dir.c_str(), nullptr);

    m_userDataDir = dir;
    return m_userDataDir;
}

bool JZWebView2Host::createWindow(const std::string &url)
{
    m_url = url;
    m_navigateUrl = utf8ToWide(url);

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &JZWebView2Host::wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"JZWebView2Host";

    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        fprintf(stderr, "[desktop] RegisterClassW failed: %lu\n", GetLastError());
        return false;
    }

    m_hwnd = CreateWindowExW(0, L"JZWebView2Host", L"JZ WebView2", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720,
                             nullptr, nullptr, instance, this);
    if (!m_hwnd) {
        fprintf(stderr, "[desktop] CreateWindowExW failed: %lu\n", GetLastError());
        return false;
    }

    ShowWindow(m_hwnd, SW_MAXIMIZE);
    UpdateWindow(m_hwnd);

    std::wstring dataDir = userDataDir();
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dataDir.c_str(), nullptr, new EnvironmentCreatedHandler(this));
    if (FAILED(hr)) {
        fwprintf(stderr, L"[desktop] CreateCoreWebView2EnvironmentWithOptions failed: 0x%08lX\n",
                 static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

HRESULT JZWebView2Host::onEnvironmentCreated(ICoreWebView2Environment *env)
{
    if (!env)
        return E_POINTER;
    if (!m_hwnd || !IsWindow(m_hwnd))
        return E_FAIL;

    m_environment = env;
    m_environment->AddRef();

    LPWSTR version = nullptr;
    if (SUCCEEDED(m_environment->get_BrowserVersionString(&version)) && version) {
        fwprintf(stderr, L"[desktop] WebView2 runtime: %ls\n", version);
        CoTaskMemFree(version);
    }

    HRESULT hr = m_environment->CreateCoreWebView2Controller(
        m_hwnd, new ControllerCreatedHandler(this));
    if (FAILED(hr)) {
        fwprintf(stderr, L"[desktop] CreateCoreWebView2Controller failed: 0x%08lX\n",
                 static_cast<unsigned long>(hr));
    }
    return hr;
}

HRESULT JZWebView2Host::onControllerCreated(ICoreWebView2Controller *controller)
{
    if (!controller)
        return E_POINTER;
    if (!m_hwnd || !IsWindow(m_hwnd))
        return E_FAIL;

    m_controller = controller;
    m_controller->AddRef();

    HRESULT hr = m_controller->get_CoreWebView2(&m_webview);
    if (FAILED(hr) || !m_webview) {
        fputs("[desktop] get_CoreWebView2 failed\n", stderr);
        return FAILED(hr) ? hr : E_FAIL;
    }

    RECT bounds = {};
    GetClientRect(m_hwnd, &bounds);
    m_controller->put_Bounds(bounds);
    m_controller->put_IsVisible(TRUE);

    NavigationCompletedHandler *navHandler = new NavigationCompletedHandler();
    EventRegistrationToken navToken = {};
    hr = m_webview->add_NavigationCompleted(navHandler, &navToken);
    if (FAILED(hr)) {
        navHandler->Release();
        fwprintf(stderr, L"[desktop] add_NavigationCompleted failed: 0x%08lX\n",
                 static_cast<unsigned long>(hr));
        return hr;
    }
    navHandler->Release(); // 事件注册自身已 AddRef, 这里放下我们的那份引用。

    hr = m_webview->Navigate(m_navigateUrl.c_str());
    if (FAILED(hr)) {
        fwprintf(stderr, L"[desktop] Navigate failed: 0x%08lX\n",
                 static_cast<unsigned long>(hr));
        return hr;
    }

    m_available = true;
    return S_OK;
}

void JZWebView2Host::shutdown()
{
    if (m_controller)
        m_controller->Close();

    safeRelease(m_webview);
    safeRelease(m_controller);
    safeRelease(m_environment);

    if (m_hwnd && IsWindow(m_hwnd)) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }

    if (m_comInitialized) {
        CoUninitialize();
        m_comInitialized = false;
    }
    m_available = false;
}

LRESULT CALLBACK JZWebView2Host::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_CREATE) {
        CREATESTRUCTW *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return 0;
    }

    JZWebView2Host *host = reinterpret_cast<JZWebView2Host *>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_SIZE:
        if (host && host->m_controller) {
            RECT bounds = {};
            GetClientRect(hwnd, &bounds);
            host->m_controller->put_Bounds(bounds);
        }
        return 0;
    case WM_CLOSE:
        if (host && host->m_controller)
            host->m_controller->Close();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
