#ifndef JZ_WIN32_THREAD_H_
#define JZ_WIN32_THREAD_H_

#include "JZThread.h"
#include "JZRect.h"
#include <vector>
#include "JZWindow.h"

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
    void dealPaint();

public:
    // 合成脏区: JZWindow 收到 invalidate 事件后调这里 (窗口所在主线程),
    // 同窗口脏区合并进 paint task, 由 run() 每轮 dealPaint 统一绘制提交。
    // 不主动 wakeUp: 调用发生在 processEvents 派发中, 本轮 dealPaint 紧随其后。
    void invalidate(JZWindow* window, const JZRect& rc);

private:

    static LRESULT CALLBACK msgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void* m_msgHwnd = nullptr;   // message-only window (target of wakeup messages)
    UINT m_wakeupMsg = 0;        // custom WM_APP wakeup message id
    std::atomic<bool> m_wakePending{ false };   // 已有唤醒消息在途 (防重复投递)

    std::vector<PaintTask> m_paintTask;
};

#endif // JZ_WIN32_THREAD_H_