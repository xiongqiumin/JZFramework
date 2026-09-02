#include "JZWin32Thread.h"
#include "JZMutex.h"
#include "JZPlatform.h"
#include <cstdint>
#include "JZWindow.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// 如果您的 Windows SDK 版本较旧（低于 10.0.15063），这些常量可能未定义，手动补上
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_UNAWARE           ((HANDLE)-1)
#define DPI_AWARENESS_CONTEXT_SYSTEM_AWARE      ((HANDLE)-2)
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE ((HANDLE)-3)
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif

// 定义函数指针类型
typedef BOOL(WINAPI *SetProcessDpiAwarenessContextPtr)(HANDLE);

void InitializeDpiAwareness()
{
    // 1. 动态获取 user32.dll 中的函数地址（避免静态链接导致 OldSystem 无法启动）
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (!hUser32)
        return;

    auto pSetDpiAwareness = (SetProcessDpiAwarenessContextPtr)
        GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");

    if (!pSetDpiAwareness)
    {
        // 系统为 Windows 10 1607 之前版本（如 Win7/8/8.1），该 API 不存在
        // 可考虑调用旧的 SetProcessDPIAware() 做基础缩放，但无法实现 Per-Monitor
        // 这里选择放弃，让系统按默认 100% 缩放处理
        return;
    }

    // 2. 优先尝试 V2（Per-Monitor V2，支持子窗口缩放，Win10 1703+ 专属）
    if (pSetDpiAwareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
        // 成功启用 V2（系统 >= 1703）
        return;
    }

    // 3. V2 失败（系统为 1607，或者发生了其他错误），回退到 V1（Per-Monitor V1）
    //    注意：1607 支持该 API 和 V1 常量，但 V2 常量未被识别，传入 -4 会返回 FALSE
    pSetDpiAwareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
}

JZWin32Thread::JZWin32Thread()
    : JZThread(JZObject::MainThreadInit())
{
    InitializeDpiAwareness();    
    m_wakeupMsg = WM_APP + 0x4A17;
    createMessageWindow();    
}

JZWin32Thread::~JZWin32Thread()
{
    if (m_msgHwnd) {
        DestroyWindow((HWND)m_msgHwnd);
        m_msgHwnd = nullptr;
    }
}

// message-only window: invisible, not enumerable as a top-level window.
// Its only job is to receive the custom wakeup message (and timers).
LRESULT CALLBACK JZWin32Thread::msgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_TIMER:
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void JZWin32Thread::createMessageWindow()
{
    if (m_msgHwnd)
        return;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = msgWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"JZMsgWndClass";
    RegisterClassExW(&wc);

    // HWND_MESSAGE parent => message-only window
    m_msgHwnd = CreateWindowExW(0, L"JZMsgWndClass", L"", 0,
                                0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                GetModuleHandleW(nullptr), nullptr);

    // 30ms heartbeat: drives framework timers (JZTimer) when the loop is idle
    if (m_msgHwnd)
        SetTimer((HWND)m_msgHwnd, 1, 30, nullptr);
}

void JZWin32Thread::wakeUp()
{
    // 已有唤醒消息在途时不重复投递: 循环每轮会把事件队列全量处理一遍,
    // 一次唤醒即可带走全部待处理事件。标记由 run() 轮首复位。
    if (m_wakePending.exchange(true))
        return;
    PostMessageW((HWND)m_msgHwnd, m_wakeupMsg, 0, 0);   // thread-safe
}

void JZWin32Thread::invalidate(JZWindow* window, const JZRect& rc)
{
    // 同窗口脏区合并; 任务在 run() 每轮 dealPaint 中统一绘制并清空
    JZMutexLocker lock(&m_mutex);
    for (auto& t : m_paintTask)
    {
        if (t.window == window)
        {
            t.rc = t.rc.united(rc);
            return;
        }
    }
    m_paintTask.push_back({ window, rc });
}

void JZWin32Thread::dealPaint()
{
    for (auto& task : m_paintTask)
        task.window->doPaint(task.rc);
    m_paintTask.clear();
}

// 一轮循环体: 全量处理框架事件 + 提交脏区绘制, run() 与 execModal() 共用。
// 轮首复位唤醒标记: 本轮会清空事件队列, 之后新到事件经 wakeUp 重新投递。
void JZWin32Thread::processAndPaint()
{
    m_wakePending = false;
    processEvents();
    dealPaint();
}

// 通用模态循环 (DialogBoxParam 语义): 可见期间泵 Win32 消息 + 处理框架事件 +
// 绘制脏区; 禁用/恢复 owner (disableOwner=false 供下拉弹层 — 主窗口保持可用,
// 外点由弹层鼠标抓取吞掉)。hide() 即退出。与 run() 共享 processAndPaint 轮体;
// 可重入 (模态中再开模态各自独立栈帧, m_paintTask 同线程顺序访问)。
int JZWin32Thread::execModal(JZWindow* window, bool disableOwner)
{
    HWND hwnd = (HWND)window->nativeHandle();
    HWND owner = hwnd ? GetWindow(hwnd, GW_OWNER) : nullptr;
    bool ownerDisabled = (disableOwner && owner && IsWindowEnabled(owner));
    if (ownerDisabled)
        EnableWindow(owner, FALSE);

    bool quit = false;
    MSG msg;
    while (!quit && window->isVisible() && window->nativeHandle())
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)   // 应用退出: 归还消息并结束
            {
                PostQuitMessage((int)msg.wParam);
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit || !window->isVisible() || !window->nativeHandle())
            break;

        processAndPaint();   // 与主循环同一套轮体: 框架事件 + 脏区绘制
        MsgWaitForMultipleObjectsEx(0, nullptr, 30, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    if (ownerDisabled)
    {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    return 0;
}

// Message loop with one-shot timer wakeups: each round processes framework
// events, arms a SetTimer for the next framework timer gap, then blocks in
// GetMessage pumping ALL thread window messages (JZWindow input/render).
void JZWin32Thread::run()
{
    { FILE* f = fopen("c:/Users/7052/Desktop/other/JZFramework/build/diag.log", "a"); if (f) { fprintf(f, "diag: run enter"); fclose(f); } }
    MSG msg;
    while (m_running)
    {
        // 1. framework event queue + expired timers + paint
        //    invalidate 事件由 JZWindow::event 正常派发时经 invalidate() 合成进
        //    m_paintTask, processAndPaint 统一绘制。
        processAndPaint();
        if (!m_running)
            break;

        // 2. arm a one-shot timer for the next framework timer gap
        int64_t gap = 1000;
        {
            JZMutexLocker lock(&m_mutex);
            int64_t now = JZPlatform::getTickCountMs();
            for (size_t i = 0; i < m_timerList.size(); ++i)
            {
                int64_t g = m_timerList[i].nextTrigger - now;
                if (g < gap)
                    gap = g;
            }
        }
        if (gap < 0) gap = 0;
        if (gap > 1000) gap = 1000;
        SetTimer((HWND)m_msgHwnd, 1, (UINT)gap, nullptr);

        // 3. block pumping one message (JZWindow input/render timer/wakeup/WM_TIMER)
        int ret = GetMessageW(&msg, nullptr, 0, 0);
        if (ret <= 0)
            break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        KillTimer((HWND)m_msgHwnd, 1);   // one-shot: re-armed next round
    }
    KillTimer((HWND)m_msgHwnd, 1);
}
