// ==================== 虚拟进程: 一个线程 + 一份 JS 运行时 + 它的窗口 ====================
// 线程模型 (见 JZVirtualProcess.h 的头注释): 与桌面的通信只有三处 ——
//   ① 建窗: 本线程 JZWindow 构造 -> 窗口宿主(内部小类 WindowHost) -> 桌面 adoptWindow;
//   ② 输入/焦点: 桌面 jzApp->sendEvent(client, ev) -> 投到本线程并等它处理完 (同步);
//   ③ 帧: 反过来 —— 本线程画完把帧推给桌面 (captureFrame), 桌面合成只读那份拷贝。
// 于是**没有**"跨线程直接摸别人的对象"的地方: 要么走框架的事件投递, 要么走桌面的锁。
#include "JZVirtualProcess.h"
#include "JZVirtualWindow.h"
#include "JZVirtualDeviceHttp.h"   // 接上 HTTP 接口的"程序管理" (见文件末尾 jzAttachVirtualProgramHost)
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
    // 进程名同时当**对象名**: 桌面/HTTP 接口据此把"它建的窗口"归到这个程序名下
    // (JZVirtualWindow::process() 是个 JZThread*, 那边不认识 JZVirtualProcess)。
    setObjectName(name);
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
bool JZVirtualProcess::start(const std::string& workDir, const std::string& script,
                             const std::string& moduleRel)
{
    if (JZThread::isRunning())
        return false;
    m_workDir = workDir;
    m_script = script;
    m_module = moduleRel;
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

    // 加载程序 (它里面通常 `new JZWindow(...)` —— 那一步就把窗口交给桌面了):
    //   · 模块 (真程序): 走一站式 init(运行根, 入口) —— 它同时登记运行根与入口模块, 于是
    //     `jzApp.applicationDirPath()` = 程序所在目录 (程序按入口目录找随它走的资源, 这条
    //     约定才成立), 出错时的栈也会按 source map 翻回源码位置;
    //   · 脚本文本: 就跑这一段 (没有模块/入口语义)。
    if (!m_module.empty()) {
        if (!m_runtime.init(m_workDir, m_module))
            setError(m_runtime.lastError());
    } else if (!m_script.empty()) {
        runScript(m_script);
    }

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
    m_workDir = wd;                  // 补全缺省 (run() 里按模块加载要用它当运行根)
    m_runtime.setLogHandler([this](const std::string& s) {
        jzInfo() << "[vproc:" << m_name << "] " << s;
    });
    m_runtime.setUnhandledExceptionHandler([this](const std::string& e) {
        setError(e);
        jzError() << "[vproc:" << m_name << "] 未处理异常: " << e;
    });
    // ⚠ 走模块那条路 (真程序) 时**不在这里建会话**: 交给一站式 init(运行根, 入口) ——
    //   它会把运行根/入口登记对, 而"程序按入口目录找随它走的资源"这条约定就落在那上面
    //   (见 run())。这里只负责把脚本文本那条路的会话建好。
    if (m_module.empty() && !m_runtime.initEngine(wd)) {
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
                                                 const std::string& workDir,
                                                 const std::string& moduleRel)
{
    JZVirtualProcess* p = new JZVirtualProcess(this, name);
    if (!p->start(workDir, script, moduleRel)) {
        delete p;
        return nullptr;
    }
    return p;
}

// ==================== 接上 HTTP 接口的"程序管理" ====================
// JZVirtualDeviceHttp 不认识引擎 (方向只能 process -> device), 所以"加载程序"这几个动作由
// 本层登记 —— 宿主一行搞定: `JZVirtualDeviceHttp api(&desk); jzAttachVirtualProgramHost(&desk, &api);`
//
// ⚠ 全部只在**调用方线程** (= 主线程, HTTP 回调所在线程) 跑; 进程表 (JZVirtualDevice::processes)
//   只在主线程增删, 所以这里直接遍历它是安全的 (见 JZVirtualDevice::addProcess 的注释)。
namespace {

JZVirtualProcess* jzFindVirtualProcess(JZVirtualDevice* desk, const std::string& name)
{
    if (!desk || name.empty())
        return nullptr;
    for (JZThread* t : desk->processes()) {
        JZVirtualProcess* p = dynamic_cast<JZVirtualProcess*>(t);
        if (p && p->name() == name)
            return p;
    }
    return nullptr;
}

} // namespace

void jzAttachVirtualProgramHost(JZVirtualDevice* desk, JZVirtualDeviceHttp* api)
{
    if (!desk || !api)
        return;

    JZVirtualProgramHost host;

    // 加载程序 = 起一个虚拟进程 (一个线程 + 一份 JS 运行时), 里面加载:
    //   moduleRel 非空 = 一个 JS 程序 (ESM 模块, 相对 workDir) —— import 能用, 且入口目录就是
    //                    它的所在目录 (jzApp.applicationDirPath()); script 非空 = 一段代码。
    // 起完**等它的第一个窗口** (最多 5 秒): 调用方拿到响应就能截图/点击, 不必自己轮询。
    // ⚠ 不建窗口的程序是合法的 (纯脚本) ⇒ 等不到窗口不算失败, 由调用方看 windows 列表。
    host.launch = [desk](const std::string& name, const std::string& script,
                         const std::string& workDir, const std::string& moduleRel,
                         std::string& error) -> bool {
        if (name.empty()) {
            error = "程序名不能为空";
            return false;
        }
        if (jzFindVirtualProcess(desk, name)) {
            error = "同名程序已在跑: " + name;
            return false;
        }
        JZVirtualProcess* p = desk->createProcess(name, script, workDir, moduleRel);
        if (!p) {
            error = "起进程失败 (线程没起来): " + name;
            return false;
        }
        if (!p->waitWindow(5000)) {
            const std::string why = p->lastError();
            jzInfo() << "[vproc] " << name << " 起来了但 (5 秒内) 没建出窗口"
                     << (why.empty() ? "" : (": " + why));
        }
        desk->refresh();
        return true;
    };

    // 在已跑的程序里再执行一段 (同步等它跑完): 自动化用来"喂"下一步脚本/取中间状态
    host.run = [desk](const std::string& name, const std::string& script, std::string& error) -> bool {
        JZVirtualProcess* p = jzFindVirtualProcess(desk, name);
        if (!p) {
            error = "没有这个程序: " + name;
            return false;
        }
        if (!p->runScript(script)) {
            error = p->lastError();
            if (error.empty())
                error = "脚本执行失败: " + name;
            return false;
        }
        desk->refresh();
        return true;
    };

    host.list = [desk]() {
        std::vector<std::string> names;
        for (JZThread* t : desk->processes()) {
            JZVirtualProcess* p = dynamic_cast<JZVirtualProcess*>(t);
            if (p)
                names.push_back(p->name());
        }
        return names;
    };

    // 停程序: terminateProcess = 关线程 + join + 收走它建的窗口 + 删对象 (全在主线程)
    host.stop = [desk](const std::string& name) -> bool {
        JZVirtualProcess* p = jzFindVirtualProcess(desk, name);
        if (!p)
            return false;
        desk->terminateProcess(p);     // ⚠ 之后 p 已失效, 不要再碰
        return true;
    };

    api->setProgramHost(host);
}
