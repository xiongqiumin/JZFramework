#ifndef JZ_WIN32_THREAD_H_
#define JZ_WIN32_THREAD_H_

#include "JZThread.h"
#include "JZRect.h"
#include <vector>
#include <atomic>
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

    // 一轮循环体: 全量处理框架事件 + 提交脏区绘制 (run 与模态循环共用)。
    // 轮首复位唤醒标记: 本轮清空事件队列, 之后新到事件经 wakeUp 重新投递。
    void processAndPaint();

    // run() 与 execModal() 共用的"处理 + 唤醒 + 阻塞"一轮:
    //   processAndPaint() -> 算 gap 并 SetTimer(gap) 唤醒 -> GetMessageW 阻塞等一条
    //   -> PeekMessageW 排空剩余。返回 false 表示收到 WM_QUIT 或线程停止(m_running=false)。
    bool pumpOnce();

public:
    // 合成脏区: JZWindow 收到 invalidate 事件后调这里 (窗口所在主线程),
    // 同窗口脏区合并进 paint task, 由每轮 dealPaint 统一绘制提交。
    // 不主动 wakeUp: 调用发生在 processEvents 派发中, 本轮 dealPaint 紧随其后。
    void invalidate(JZWindow* window, const JZRect& rc);

    // 通用模态循环 (DialogBoxParam 语义): 可见期间泵 Win32 消息 + 处理框架
    // 事件 + 绘制脏区; 禁用/恢复 owner (disableOwner=false 供下拉弹层用,
    // 主窗口保持可用, 外点由弹层鼠标抓取吞掉)。hide() 即退出。
    // 与 run() 共享 processAndPaint 轮体; 可重入 (模态中再开模态独立栈帧)。
    int execModal(JZWindow* window, bool disableOwner = true);

private:

    static LRESULT CALLBACK msgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void* m_msgHwnd = nullptr;   // message-only window (target of wakeup messages)
    UINT m_wakeupMsg = 0;        // custom WM_APP wakeup message id
    std::atomic<bool> m_wakePending{ false };   // 已有唤醒消息在途 (防重复投递)

    std::vector<PaintTask> m_paintTask;
};

#endif // JZ_WIN32_THREAD_H_