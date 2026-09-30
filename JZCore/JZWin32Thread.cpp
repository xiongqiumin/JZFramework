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
    // 先把本轮任务摘出来再画: doPaint 里可能又产生脏区(同步派发的 invalidate),
    // 那些落到**下一轮**, 不能往正在遍历的容器里塞(迭代器失效)。
    std::vector<PaintTask> tasks;
    {
        JZMutexLocker lock(&m_mutex);
        tasks.swap(m_paintTask);
    }
    for (auto& task : tasks)
    {
        if (task.window)
            task.window->doPaint(task.rc);
    }
}

// 一轮循环体: 全量处理框架事件 + 提交脏区绘制, run() 与 execModal() 共用。
// 轮首复位唤醒标记: 本轮会清空事件队列, 之后新到事件经 wakeUp 重新投递。
void JZWin32Thread::processAndPaint()
{
    m_wakePending = false;
    processEvents();
    dealPaint();
}

int JZWin32Thread::execLoop(JZEventLoop *loop)
{
    while (loop->isRunning())
    {
        if (!pumpOnce())
            break;
    }
    return loop->code();
}

// 通用模态循环 (DialogBoxParam 语义): 可见期间泵 Win32 消息 + 处理框架事件 +
// 绘制脏区; 禁用/恢复 owner (disableOwner=false 供下拉弹层 — 主窗口保持可用,
// 外点由弹层鼠标抓取吞掉)。hide() 即退出。与 run() 共用 pumpOnce() 轮体(同样按框架
// 定时器 gap 唤醒, 模态内游戏定时器也能精确触发); 可重入 (模态中再开模态独立栈帧)。
int JZWin32Thread::execModal(JZWindow* window, bool disableOwner)
{
    HWND hwnd = (HWND)window->nativeHandle();
    HWND owner = hwnd ? GetWindow(hwnd, GW_OWNER) : nullptr;
    bool ownerDisabled = (disableOwner && owner && IsWindowEnabled(owner));
    if (ownerDisabled)
        EnableWindow(owner, FALSE);

    while (window->isVisible() && window->nativeHandle())
    {
        // 退出条件: 窗口隐藏(正常关弹层) 或 线程停止/收到 WM_QUIT。
        // pumpOnce 在 WM_QUIT / app.quit() 时都已置 m_running=false, run() 循环靠该标志退出,
        // 此处无需再 PostQuitMessage(否则嵌套模态下会按层数多发且冗余)。
        if (!pumpOnce())
            break;
    }

    if (ownerDisabled)
    {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    return 0;
}

// 弹层(Popup)的"外部按下关闭": 在派发鼠标按下消息前调用。
// 以前靠 Popup 窗口各自 SetCapture 抓到客户区外的按下再关窗 —— 但抓取是全局唯一的,
// 嵌套弹层(菜单的子菜单)会互相抢走, 抓取窗口之外再也收不到鼠标消息(悬停/选择失效)。
// 现改为按屏幕坐标统一判定: 见 JZWindow::filterPopupPress。返回 true 表示已消费(吞掉)。
static bool jzPopupPressFilter(const MSG& msg)
{
    const UINT m = msg.message;
    if (m != WM_LBUTTONDOWN && m != WM_RBUTTONDOWN && m != WM_MBUTTONDOWN &&
        m != WM_NCLBUTTONDOWN && m != WM_NCRBUTTONDOWN && m != WM_NCMBUTTONDOWN)
        return false;
    return JZWindow::filterPopupPress(msg.pt.x, msg.pt.y);
}

// 鼠标移动合并: 高刷鼠标每秒能产生上百条 WM_MOUSEMOVE, 而**中间位置对框架没有
// 任何意义** —— 位置由最后一条决定 (拖拽锚点也记在按下那一刻, 见 JZSplitter::mouseMoveEvent),
// 每条都跑一遍"命中测试 + 重排 + 业务槽"纯属浪费, 一旦跟不上就积压成"拖不动"。
// 这里把本窗口队列里后续的 WM_MOUSEMOVE 全部取出丢掉, 只保留最新坐标的那条派发
// (等价 Qt 的做法: 处理前先 PeekMessage 把同窗口的鼠标移动压成最后一条)。
// 其它消息 (按键/重绘/定时器) 不受影响, 仍按原顺序留在队列里。
static void jzMergePendingMouseMove(MSG& msg)
{
    if (msg.message != WM_MOUSEMOVE || !msg.hwnd)
        return;

    MSG next;
    while (PeekMessageW(&next, msg.hwnd, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE))
        msg = next;         // 保留最新位置 (lParam/pt 已是最新)
}

// 共享一轮: 处理框架事件+绘制 -> 算 gap 并 SetTimer 唤醒 -> GetMessageW 阻塞等一条
// -> PeekMessageW 排空剩余。返回 false 表示 WM_QUIT 或线程停止(m_running=false)。
bool JZWin32Thread::pumpOnce()
{
    // 1. 框架事件队列 + 到期定时器 + 脏区绘制
    //    invalidate 事件由 JZWindow::event 正常派发时经 invalidate() 合成进
    //    m_paintTask, processAndPaint 统一绘制。
    processAndPaint();
    if (!m_running)
        return false;

    // 2. 算下一个框架定时器的间隔, 启动(重置)唤醒定时器
    //    定时器本身不触发逻辑, 只给消息泵"发 WM_TIMER 唤醒", 回到 processTimers 处理。
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
    if (gap < 0) gap = 0;            // 已到期: 立即唤醒
    else if (gap == 0) gap = 1;      // 0 间隔: 留 1ms 让出, 否则 GetMessage 立即返回忙转饿死 GUI
    if (gap > 1000) gap = 1000;
    SetTimer((HWND)m_msgHwnd, 1, (UINT)gap, nullptr);

    // 3. 阻塞等到一条消息(定时器唤醒/输入); GetMessageW 返回即代表有消息可派发
    MSG msg;
    if (!GetMessageW(&msg, nullptr, 0, 0))
    {
        m_running = false;          // WM_QUIT(0) 或错误(-1)
        return false;
    }
    jzMergePendingMouseMove(msg);   // 鼠标移动: 只派发最新一条
    if (!jzPopupPressFilter(msg))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 4. 本轮队列里剩余消息一次性排空(输入/定时器/重绘), 输入延迟更低
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT) { m_running = false; return false; }
        jzMergePendingMouseMove(msg);   // 同上: 同一批里只留最新的移动
        if (jzPopupPressFilter(msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

// 消息泵: 复用 pumpOnce() 循环体, 由 m_running 控制退出。
void JZWin32Thread::run()
{
    while (m_running)
    {
        if (!pumpOnce())
            break;
    }
    KillTimer((HWND)m_msgHwnd, 1);
}
