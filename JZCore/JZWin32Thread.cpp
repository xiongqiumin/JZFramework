#include "JZWin32Thread.h"

#include "JZMutex.h"
#include "JZPlatform.h"
#include <cstdint>

#ifdef HAS_JZWidget
#include "JZWindow.h"
#endif

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

JZWin32Thread::JZWin32Thread()
    : JZThread(JZObject::MainThreadInit())
{
    { FILE* f = fopen("c:/Users/7052/Desktop/other/JZFramework/build/diag.log", "a"); if (f) { fprintf(f, "diag: JZWin32Thread ctor enter"); fclose(f); } }
    m_wakeupMsg = WM_APP + 0x4A17;
    createMessageWindow();
    { FILE* f = fopen("c:/Users/7052/Desktop/other/JZFramework/build/diag.log", "a"); if (f) { fprintf(f, "diag: msgwnd created"); fclose(f); } }
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

// Message loop with one-shot timer wakeups: each round processes framework
// events, arms a SetTimer for the next framework timer gap, then blocks in
// GetMessage pumping ALL thread window messages (JZWindow input/render).
void JZWin32Thread::run()
{
    { FILE* f = fopen("c:/Users/7052/Desktop/other/JZFramework/build/diag.log", "a"); if (f) { fprintf(f, "diag: run enter"); fclose(f); } }
    MSG msg;
    while (m_running)
    {
        // 复位唤醒标记: 本轮 processEvents 会全量处理事件队列, 此刻之后
        // (轮内/阻塞期间) 新到的事件经 wakeUp 重新投递唤醒消息
        m_wakePending = false;

        // 1. framework event queue + expired timers
        //    invalidate 事件由 JZWindow::event 正常派发时经 invalidate() 合成进
        //    m_paintTask (不再扫队列抽取), 本轮 dealPaint 统一绘制。
        processEvents();
        dealPaint();
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
#endif // _WIN32
