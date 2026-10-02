#ifndef JZ_VIRTUAL_PROCESS_H_
#define JZ_VIRTUAL_PROCESS_H_
#include "JZVirtualDevice.h"      // 桌面 (它把进程就当一个 JZThread 管)
#include "JZWindowHost.h"         // 进程内部那个窗口宿主适配器 (见下面的 WindowHost)
#include "JZEngineRuntime.h"      // 本进程的 JS 运行时 (内含一个 JZJSEngine)
#include "JZThread.h"
#include "JZMutex.h"
#include <atomic>
#include <string>

class JZVirtualWindow;
class JZWindow;
class JZVirtualDeviceHttp;

// ==================== 虚拟进程 (一个窗口一个线程) ====================
// 一台虚拟机上的"进程": **一个 JZThread + 一个 JZEngineRuntime(内含 JZJSEngine)**, 里面
// 跑一份 JS。JS 里 `new JZWindow(...)` 建出来的窗口就归这个进程 —— 于是每个窗口是独立
// 线程、独立 JS 运行时, 一个窗口卡死不拖累别的窗口, 崩了也只崩自己(=自己退出, 见下;
// JZVirtualDevice 的进程管理因此成立)。
//
// 它**只继承 JZThread** (不搞多继承): 对桌面来说进程就是一个线程 —— 停/等/查都在
// JZThread 上; "窗口宿主"这层需要用 JZWindowHost 接口, 由一个内部小类 WindowHost 承担
// (见下), 转发到本进程。
//
// ⚠ **未处理异常 = 本进程退出** (与真系统同语义: JS 顶层错误/未决 Promise 拒绝 = 程序失败)。
//   链路: 引擎 sigError -> JZEngineRuntime 的桥 -> initRuntime 里登记的 handler
//        -> setError() + JZThread::quit()      (只请求退出, 不在这里碰别的线程)
//        -> run() 从事件循环出来, destroyRuntime() 后线程结束
//        -> finishProcess() 往主线程投一个异步事件 -> 桌面 terminateProcess (摘表 + 收窗口 + delete)
//   于是 /api/programs 里它会消失、窗口一起收走 —— 不留"装死占着窗口"的进程。
//   注意引擎在异常时已经 stopEngineTimer, 所以这个进程不会自己复活: 要重跑就重新加载程序。
//   (另外: POST /api/programs 加载"起来就崩"的程序会直接报 400 + 错误文本, 见 launch)
//
// 线程模型 (与桌面的全部通信, 都不是新协议, 用的都是框架现成机制):
//   · 建窗: 本线程里 JZWindow 构造的最后一步回调**本进程的窗口宿主** —— 它把窗口交给
//     桌面收编 (JZVirtualDevice::adoptWindow, 桌面内部一把递归锁);
//   · 注入输入(点/键/滚轮)/ 焦点切换: 桌面调 jzApp->sendEvent(clientWindow, ev) ——
//     跨线程会投递并**阻塞等**本线程处理完 (同步 RPC), 顺序/返回值与单线程一致;
//   · 取帧: 不需要跨线程读别人的帧缓冲 —— 窗口画完就由本线程把帧**推**给桌面
//     (JZVirtualWindowBackend::onPainted -> captureFrame), 桌面合成只读那份拷贝;
//   · 停进程: quit()(协作式) + wait()(join); 引擎在本线程析构 (JSRuntime 只能在建它的
//     线程里用, 见 destroyRuntime);
//   · 进程**自己**结束 (未处理异常 / 线程体跑完): 本线程往桌面投一个异步事件 -> 桌面
//     terminateProcess 收尾 (摘表 + 收窗口), 见 finishProcess。于是桌面不需要"定期清扫
//     死进程", 程序表里也不会留一个跑完了的进程。
//
// 用法 (宿主/测试):
//     JZVirtualProcess* p = desk.createProcess("app1", "JS 源码");
//     p->waitWindow(3000);                       // 等它把窗口建出来 (只保证已显示)
//     desk.injectClickAt(x, y);                  // 点它 —— 事件会落到它自己的线程
//     desk.terminateProcess(p);                  // 停掉它 (窗口一起收走)
class JZVirtualProcess : public JZThread
{
public:
    JZVirtualProcess(JZVirtualDevice* device, const std::string& name);
    ~JZVirtualProcess() override;

    // 启动: 本线程里建运行时 + 装窗口宿主, 然后**加载程序** (二选一, 与 code_runtime 的两条入口一致):
    //   workDir  : 运行根 (引擎找模块/相对路径的基准; 空 = 进程当前目录)
    //   moduleRel: 非空 = 以 **ESM 模块**加载它 (相对 workDir, 扩展名可省) —— 跑"真程序"用这条:
    //              `import` 能用, 而且入口模块会被登记成 appFile ⇒ `jzApp.applicationDirPath()`
    //              就是**程序所在目录** (脚本按入口目录找随它走的资源, 这条约定才成立)。
    //   script   : 非空 = 当一段 JS 跑 (全局空间执行, 适合片段/脚本体; 没有模块语义、也没有入口目录)
    // 两个都为空 = 只建会话不跑。返回 false = 线程没起来; 程序本身的错误见 lastError()。
    bool start(const std::string& workDir, const std::string& script,
               const std::string& moduleRel = std::string());
    // 在本进程线程上执行一段 JS (同步等它跑完); 失败见 lastError()
    bool runScript(const std::string& script);

    JZVirtualDevice* device() const { return m_device; }
    const std::string& name() const { return m_name; }
    int processId() const { return m_pid; }

    // ---- 窗口 ----
    // 本进程建的窗口 (外框对象); 还没建好时为空。第一个窗口是"本进程的窗口"。
    JZVirtualWindow* window() const;
    JZWindow* windowClient() const;
    // 等第一个窗口建出来 (且已显示)。
    // ⚠ 只保证"可见"; 返回后**别立刻**对那个窗口做跨线程同步的动作(注入输入 / 取帧 / RPC) ——
    //   那时窗口线程可能还在跑 show 排下来的活, 两边互等会死锁。原因与出路见 .cpp 实现上那段。
    bool waitWindow(int timeoutMs);

    // ---- 运行状态 / 错误 ----
    std::string lastError() const;
    bool ok() const { return !m_error; }
    // run() 是否已经跑完 (线程已结束 / 正在结束)。⚠ 不能用 JZThread::isRunning() 代替:
    // 它的判据是 std::thread 还 joinable, 而线程函数返回后**仍然 joinable**(要等 join) ——
    // 所以"结束了没有"只能由进程自己在 run() 末尾置位 (见 run/waitWindow)。
    bool isFinished() const { return m_finished; }

    // 窗口宿主把"本线程新建的窗口"转给这里 (见 WindowHost); 也是窗口归属的登记点
    void onWindowCreated(JZWindow* w);

protected:
    void run() override;                     // 线程体: 建运行时 -> 装宿主 -> 跑脚本 -> 事件循环

private:
    // 窗口宿主: "这个线程里 new 出来的窗口归谁托管" (见 JZWindowHost.h)。
    // 单独一个小类而不是让 JZVirtualProcess 多继承 —— 进程只需要是 JZThread。
    class WindowHost : public JZWindowHost
    {
    public:
        explicit WindowHost(JZVirtualProcess* process) : m_process(process) {}
        void onWindowCreated(JZWindow* w) override;

    private:
        JZVirtualProcess* m_process;
    };

    bool initRuntime();                      // 在本线程里建运行时/引擎
    void destroyRuntime();                   // 在本线程里销毁 (JSRuntime 有线程亲和)
    void finishProcess();                    // run() 收尾: 置 isFinished + 通知桌面收尾
    void setError(const std::string& text);

    JZVirtualDevice* m_device = nullptr;
    std::string m_name;
    std::string m_workDir;
    std::string m_script;                    // 当脚本文本跑 (见 start)
    std::string m_module;                    // 以模块加载 (见 start); 非空则优先
    int m_pid = 0;
    WindowHost m_host{this};                 // 只在本线程里被用 (见 initRuntime)

    // ⚠ 运行时**只在本线程里用** (JZEngineRuntime 里是 JSRuntime, 线程亲和):
    //   字段本身在主线程构造, 但 init/destroy 都在 run() 里跑。
    JZEngineRuntime m_runtime;
    bool m_runtimeReady = false;

    mutable JZMutex m_lock;                  // 保护下面三样 (窗口/错误)
    JZObjectPointer m_clientGuard;           // 本进程窗口的**客户区**存活守卫
    JZVirtualWindow* m_window = nullptr;     // 客户区还活着 ⇒ 这个也还在 (同一批生命周期)
    std::string m_lastError;
    bool m_error = false;
    std::atomic<bool> m_finished{false};     // run() 跑完了 (见 isFinished)
};

// ==================== 与 HTTP 接口对接 ====================
// 把"加载程序 / 列程序 / 停程序"装到 JZVirtualDeviceHttp 的宿主钩子上
// (那个层不认识 JS 引擎, 见 JZVirtualDeviceHttp.h 的纪律 5)。宿主一行:
//     JZVirtualDeviceHttp api(&desk);
//     jzAttachVirtualProgramHost(&desk, &api);
// ⚠ 只允许在主线程调用 (进程表只在主线程增删); 之后 HTTP 的 /api/programs 就能用了。
//   不接 = 那些接口回 501, 容器其余部分 (截图/输入/窗口) 照常可用。
void JZFRAMEWORK_API jzAttachVirtualProgramHost(JZVirtualDevice* desk, JZVirtualDeviceHttp* api);

#endif // JZ_VIRTUAL_PROCESS_H_
