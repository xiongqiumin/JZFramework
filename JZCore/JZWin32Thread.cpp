#include "JZWin32Thread.h"

#include "JZMutex.h"
#include "JZPlatform.h"
#include <cstdint>

#ifdef HAS_JZWidget
#include "JZPainterEvent.h"
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
    PostMessageW((HWND)m_msgHwnd, m_wakeupMsg, 0, 0);   // thread-safe
}

void JZWin32Thread::collectInvaildEvents()
{
#ifdef HAS_JZWidget
    // 从框架事件队列抽取 JZEvent_invalidate, 转成 paint task (同窗口合并脏区)
    std::queue<JZEventInfo> keep;
    {
        JZMutexLocker lock(&m_mutex);
        while (!m_eventList.empty())
        {
            JZEventInfo info = m_eventList.front();
            m_eventList.pop();

            if (info.event && info.event->type() == JZEvent_invalidate)
            {
                JZInvalidateEvent* ev = (JZInvalidateEvent*)info.event;
                JZWindow* win = (JZWindow*)info.object.object();
                if (win)
                {
                    bool merged = false;
                    for (auto& t : m_paintTask)
                    {
                        if (t.window == win)
                        {
                            t.rc = t.rc.united(ev->rect);
                            merged = true;
                            break;
                        }
                    }
                    if (!merged)
                        m_paintTask.push_back({ win, ev->rect });
                }
                delete ev;
                if (info.sem)
                    info.sem->release();
                continue;
            }

            keep.push(info);   // 其它事件留给 processEvents 正常处理
        }
        m_eventList.swap(keep);
    }
#endif
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
        // 1. framework event queue + expired timers
        m_paintTask.clear();
        collectInvaildEvents();
        processEvents();
        collectInvaildEvents();
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
