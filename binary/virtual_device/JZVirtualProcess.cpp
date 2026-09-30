// ==================== 虚拟进程: 一个线程 + 一份 JS 运行时 + 它的窗口 ====================
// 线程模型 (见 JZVirtualProcess.h 的头注释): 与桌面的通信只有三处 ——
//   ① 建窗: 本线程 JZWindow 构造 -> 窗口宿主(内部小类 WindowHost) -> 桌面 adoptWindow;
//   ② 输入/焦点: 桌面 jzApp->sendEvent(client, ev) -> 投到本线程并等它处理完 (同步);
//   ③ 帧: 反过来 —— 本线程画完把帧推给桌面 (captureFrame), 桌面合成只读那份拷贝。
// 于是**没有**"跨线程直接摸别人的对象"的地方: 要么走框架的事件投递, 要么走桌面的锁。
#include "JZVirtualProcess.h"
#include "JZVirtualWindow.h"
#include "JZVirtualThread.h"   // 跨线程同步调用 (jzRunOnThreadSync)
#include "JZWindow.h"
#include "JZApplication.h"
#include "JZDir.h"
#include "JZDebug.h"    // jzInfo / jzError
#include "JZPlatform.h"

#include <algorithm>
#include <atomic>

namespace {

std::atomic<int> g_processSeq(1);

} // namespace

JZVirtualProcess::JZVirtualProcess(JZVirtualDevice* device, const std::string& name)
    : m_device(device), m_name(name)
{
    m_pid = g_processSeq.fetch_add(1);
}

JZVirtualProcess::~JZVirtualProcess()
{
    // 兜底: 线程不该还活着 (调用方要先 terminateProcess), 否则 JSRuntime 会被在错误的
    // 线程里析构 (run() 里的 destroyRuntime 才是正确位置)。这里再请求退出并等它结束。
    if (JZThread::isRunning()) {
        JZThread::quit();
        JZThread::wait();
    }
}

std::string JZVirtualProcess::lastError() const
{
    JZMutexLocker lk(&m_lock);
    return m_lastError;
}

void JZVirtualProcess::setError(const std::string& text)
{
    JZMutexLocker lk(&m_lock);
    if (m_lastError.empty())
        m_lastError = text.empty() ? std::string("unknown error") : text;
    m_error = true;
}

// ==================== 启动 / 停止 ====================
// ⚠ 停/等/查都在 JZThread 上 (quit = 请求退出, wait = join, isRunning) —— 桌面那边也只用
//   这几个, 所以进程不需要任何基类。
bool JZVirtualProcess::start(const std::string& workDir, const std::string& script)
{
    if (JZThread::isRunning())
        return false;
    m_workDir = workDir;
    m_script = script;
    if (m_device)
        m_device->addProcess(this);      // 桌面登记 (进程管理)
    JZThread::start();                   // 线程体 = run()
    return true;
}

// 窗口宿主 -> 本进程: 本线程里新建的窗口交给桌面收编 (见 WindowHost)
void JZVirtualProcess::WindowHost::onWindowCreated(JZWindow* w)
{
    if (m_process)
        m_process->onWindowCreated(w);
}

// ==================== 线程体 ====================
void JZVirtualProcess::run()
{
    if (!initRuntime()) {
        jzError() << "[vproc] " << m_name << " 运行时起不来: " << lastError();
        jzSetWindowHost(nullptr);
        return;                          // 线程立刻结束 (等待方由 waitWindow/isRunning 判定)
    }

    // 跑这段 JS (它里面通常 `new JZWindow(...)` —— 那一步就把窗口交给桌面了)
    if (!m_script.empty())
        runScript(m_script);

    // 事件循环: JS 定时器、跨线程投进来的输入事件、runOnThread 的 RPC 都在这里处理
    JZThread::exec();

    destroyRuntime();
}

bool JZVirtualProcess::initRuntime()
{
    // ① 本线程的窗口宿主 = 本进程的那个适配器: 之后这个线程里 new 出来的窗口都交给
    //    桌面收编 (宿主是线程局部的, 所以别的进程/主线程各有各的, 互不顶掉)
    jzSetWindowHost(&m_host);

    // ② 运行时(内含一个 JZJSEngine)。JSRuntime 线程亲和 ⇒ 必须在本线程里建。
    std::string wd = m_workDir.empty() ? JZDir::currentPath() : m_workDir;
    m_runtime.setLogHandler([this](const std::string& s) {
        jzInfo() << "[vproc:" << m_name << "] " << s;
    });
    m_runtime.setUnhandledExceptionHandler([this](const std::string& e) {
        setError(e);
        jzError() << "[vproc:" << m_name << "] 未处理异常: " << e;
    });
    if (!m_runtime.initEngine(wd)) {
        setError(m_runtime.lastError());
        return false;
    }
    m_runtimeReady = true;
    return true;
}

void JZVirtualProcess::destroyRuntime()
{
    // 窗口先摘掉: 客户区对象归 JS (引擎析构时随 GC 回收), 桌面那边 prune 会收尾。
    JZVirtualWindow* w = nullptr;
    {
        JZMutexLocker lk(&m_lock);
        w = m_window;
        m_window = nullptr;
        m_clientGuard = JZObjectPointer();
    }
    if (w && w->client())
        w->hide();                       // 隐藏即可 (客户区由 JS 侧回收)

    if (m_runtimeReady) {
        m_runtime.shutdownEngine();       // ⚠ 必须在本线程 (JSRuntime 线程亲和)
        m_runtimeReady = false;
    }
    jzSetWindowHost(nullptr);
}

// ==================== 窗口 ====================
// 本进程里 new 出来的窗口: 交给桌面收编 (跨线程安全), 并记住"本进程的窗口"。
void JZVirtualProcess::onWindowCreated(JZWindow* w)
{
    if (!m_device || !w)
        return;
    JZVirtualWindow* vw = m_device->adoptWindow(w, this);
    if (!vw)
        return;

    {
        JZMutexLocker lk(&m_lock);
        if (!m_window) {                 // 第一个窗口 = 本进程的窗口
            m_window = vw;
            m_clientGuard = w->guardPointer();
        }
    }
    // 注意: 这里**不** show()。显示与否由 JS 决定 (`win.show()`, 与真窗口同一套语义);
    //       show 在后端里做"摆位 + 出首帧 + 置顶聚焦", 那一步也在本线程上跑。
}

JZVirtualWindow* JZVirtualProcess::window() const
{
    JZMutexLocker lk(&m_lock);
    if (!m_clientGuard)                  // 客户区没了 ⇒ 窗口也没了 (两者同一批生命周期)
        return nullptr;
    return m_window;
}

JZWindow* JZVirtualProcess::windowClient() const
{
    JZVirtualWindow* w = window();
    return w ? w->client() : nullptr;
}

// 等"窗口已建出来且已显示" (JS 是异步跑的, 建窗发生在进程线程里)。
bool JZVirtualProcess::waitWindow(int timeoutMs)
{
    const int64_t end = JZPlatform::getTickCountMs() + (timeoutMs > 0 ? timeoutMs : 3000);
    for (;;) {
        JZVirtualWindow* w = window();
        if (w && w->isVisible())
            return true;
        if (!JZThread::isRunning())
            return w != nullptr;         // 线程结束了: 有就有, 没有不会再有
        if (JZPlatform::getTickCountMs() >= end)
            return w != nullptr;         // 超时: 建出来了就算成功 (可能还没 show)
        JZThread::msleep(5);
    }
}

// ==================== 脚本 ====================
bool JZVirtualProcess::runScript(const std::string& script)
{
    if (script.empty())
        return true;
    // 别的线程调用 -> 投到本进程线程上跑 (同步等结果)
    if (JZThread::currentThread() != this) {
        auto ok = std::make_shared<bool>(false);      // 超时后 lambda 仍可能跑: 别捕栈引用
        if (!jzRunOnThreadSync(this, [this, script, ok]() { *ok = runScript(script); }, 5000))
            return false;
        return *ok;
    }
    if (!m_runtimeReady) {
        setError("engine not ready");
        return false;
    }
    if (!m_runtime.evalScript(script)) {
        setError(m_runtime.lastError());
        return false;
    }
    return true;
}

// ==================== 桌面的工厂 (声明在 JZVirtualDevice.h) ====================
JZVirtualProcess* JZVirtualDevice::createProcess(const std::string& name,
                                                 const std::string& script,
                                                 const std::string& workDir)
{
    JZVirtualProcess* p = new JZVirtualProcess(this, name);
    if (!p->start(workDir, script)) {
        delete p;
        return nullptr;
    }
    return p;
}
