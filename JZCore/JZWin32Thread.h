#ifndef JZ_WIN32_THREAD_H_
#define JZ_WIN32_THREAD_H_

#ifdef _WIN32

#include "JZThread.h"
#include "JZRect.h"
#include <vector>

#ifdef HAS_JZWidget
#include "JZWindow.h"
#include "JZPainterEvent.h"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Windows main thread: creates a message-only window and runs the standard
// Windows message loop in run(). All window messages of this thread (JZWindow
// input/render timers) are pumped here; framework events wake the loop via
// wakeUp() posting a custom message.
class JZWin32Thread : public JZThread
{
public:
    JZWin32Thread();
    ~JZWin32Thread();

    // Wake the message loop by posting a custom message (thread-safe)
    void wakeUp() override;

protected:
    void run() override;         // GetMessage message loop (one-shot timer wakeups)

private:
    struct PaintTask
    {
        JZWindow *window;
        JZRect rc;
    };

    void createMessageWindow();
    void collectInvaildEvents();
    void dealPaint();

    static LRESULT CALLBACK msgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void* m_msgHwnd = nullptr;   // message-only window (target of wakeup messages)
    UINT m_wakeupMsg = 0;        // custom WM_APP wakeup message id

    std::vector<PaintTask> m_paintTask;
};

#endif // _WIN32

#endif // JZ_WIN32_THREAD_H_