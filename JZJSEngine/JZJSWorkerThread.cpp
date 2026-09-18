#include "JZJSWorkerThread.h"

#include <cstdio>

#include "JZApplication.h"
#include "JZJSCoreModule.h"
#include "JZJSEngine.h"
#include "JZJSNetModule.h"
#include "JZJSWorkerModule.h"
#include "JZLog.h"

// worker 内部追踪开关: 排查"消息不通/线程不退出"时改成 1, 往 stderr 打全流程。
#define JZW_TRACE 0
#if JZW_TRACE
static void wtrace(const std::string& s)
{
    fprintf(stderr, "[wtrace] %s\n", s.c_str());
    fflush(stderr);
}
#else
static void wtrace(const std::string&) {}
#endif

// 当前线程的 worker id(每线程一份)。
// 由该线程上的引擎初始化(module->initEngine)写入: 主线程写 0, worker 线程写自己的 id。
static thread_local int t_workerId = -1;

// =========================================================================
// 事件
// =========================================================================
JZJSWorkerMessageEvent::JZJSWorkerMessageEvent(int from, const JZByteArray& bytes)
    : JZEvent(JZJSWorkerEvent_Message)
{
    fromId = from;
    data = bytes;
}

JZJSWorkerExitEvent::JZJSWorkerExitEvent(int id, int code)
    : JZEvent(JZJSWorkerEvent_Exit)
{
    workerId = id;
    this->code = code;
}

// =========================================================================
// JZJSWorkerThread
// =========================================================================
JZJSWorkerThread::JZJSWorkerThread(JZObject* parent)
    : JZThread(parent)
{
    m_id = -1;
    m_parentId = -1;
    m_exitCode = 0;
    m_engine = nullptr;
    m_module = nullptr;
    m_coreModule = nullptr;
    m_netModule = nullptr;
}

JZJSWorkerThread::~JZJSWorkerThread()
{
}

void JZJSWorkerThread::setup(int id, int parentId,
                                const std::string& entry, const JZByteArray& workerData,
                                const std::string& workDir, const std::vector<std::string>& libraryDirs,
                                JZJSEngine* main)
{
    m_id = id;
    m_parentId = parentId;
    m_entry = entry;
    m_workerData = workerData;
    m_workDir = workDir;
    m_libraryDirs = libraryDirs;
    m_main = main;
}

bool JZJSWorkerThread::waitReady(int timeoutMs)
{
    return m_ready.tryAcquire(1, timeoutMs);
}

void JZJSWorkerThread::markReady()
{
    m_ready.release();
}

int JZJSWorkerThread::currentWorkerId()
{
    return t_workerId;
}

int JZJSWorkerThread::currentWorkerIdOrMain()
{
    return t_workerId < 0 ? 0 : t_workerId;
}

void JZJSWorkerThread::setCurrentWorkerId(int id)
{
    t_workerId = id;
}

void JZJSWorkerThread::reportError(const std::string& text)
{
    if (text.empty())
        return;

    // worker 引擎的错误不该静默: 打日志 + 记下来(创建方在 sigFinished 后读 lastError)
    // + 并回引擎自己的错误链(sigLog/sigError)
    LOG_E(std::string("JZJSWorker[") + std::to_string(m_id) + "] " + text);
    m_error = text;
    if (m_engine)
        m_engine->reportUncaughtError(text);
}

bool JZJSWorkerThread::buildEngine()
{
    JZJSEngine* engine = new JZJSEngine();
    engine->setWorkDir(m_workDir);
    engine->setLibraryDir(m_libraryDirs);
    engine->init();
    // 关键: init() 会让"第一个 init 的引擎"把自己当主引擎 —— worker 引擎必须被显式纠正成
    // "主引擎是主线程那个", 否则 worker 里 postTo(0, ...) 会投回自己(spawnWorker/postToWorker
    // 都靠 m_mainEngine 收敛到主引擎的 worker 表)。
    if (m_main)
        engine->setMainEngine(m_main);

    // 内置模块: core(fs/path/process/os) + net(worker 里也能跑 TCP/HTTP —— 底层
    // JZNetThread 是进程级单例, 这里只是各引擎一份上层包装)。不装 widget(GUI 只能主线程)。
    //
    // 【顺序很关键】JZJSNetModule::initEngine 会**无条件**新建并覆盖全局 __jz
    // (见 JZJSNetModule.cpp "---- __jz 命名空间 ----"), 所以往 __jz 塞子命名空间的
    // 模块必须排在它后面 —— JZJSWorkerModule 最后装。
    JZJSCoreModule* core = new JZJSCoreModule();
    core->initEngine(engine);

    JZJSNetModule* net = new JZJSNetModule();
    net->initEngine(engine);

    JZJSWorkerModule* module = new JZJSWorkerModule();   // 最后: 往 __jz 里塞 worker
    module->initEngine(engine, m_id, m_parentId, m_workerData);

    engine->start();

    m_engine = engine;
    m_module = module;
    m_coreModule = core;
    m_netModule = net;
    return true;
}

void JZJSWorkerThread::destroyEngine()
{
    // 原则: 模块先于引擎卸载; 全部在 worker 线程内完成
    // (JSRuntime 只能在建它的线程里释放; JZObject 析构也有线程断言)
    if (m_netModule)
    {
        m_netModule->deinitEngine();
        delete m_netModule;
        m_netModule = nullptr;
    }
    if (m_coreModule)
    {
        m_coreModule->deinitEngine();
        delete m_coreModule;
        m_coreModule = nullptr;
    }

    if (m_engine)
        m_engine->stop();

    if (m_module)
    {
        m_module->deinitEngine();
        delete m_module;
        m_module = nullptr;
    }
    if (m_engine)
    {
        m_engine->deinit();
        delete m_engine;
        m_engine = nullptr;
    }
}

void JZJSWorkerThread::run()
{
    t_workerId = m_id;
    wtrace("run start id=" + std::to_string(m_id) + " entry=" + m_entry);

    JZJSEngine* main = m_main;   // 主引擎(setup 时传进来的): worker 表在它那儿
    if (!buildEngine())
    {
        destroyEngine();
        m_exitCode = 1;     // 返回后框架 emit sigFinished -> 主引擎在连接里回收
        return;
    }
    wtrace("run engine built id=" + std::to_string(m_id));

    // 引擎 + 模块 + __jz 就绪: 登记投递落点并放行创建方
    // (spawnWorker 返回后 postToWorker 一定能命中)
    if (main)
        main->attachWorkerEngine(m_id, m_engine);
    markReady();

    // 入口脚本(ESM, 相对 workDir 解析)。
    // 脚本里通常第一件事就是 __jz.worker.onMessage(...) —— 这里同步跑完,
    // 之后才进事件循环收消息, 所以不会漏掉先到的消息。
    if (!m_entry.empty() && !m_engine->runModuleFile(m_entry))
    {
        std::string err = m_engine->runtimeError();
        if (err.empty())
            err = "worker entry script failed to load: " + m_entry;
        m_exitCode = 2;
        reportError(err);
    }
    wtrace("run entry done id=" + std::to_string(m_id) + " running=" + std::to_string((int)m_running));

    // 入口脚本跑完了(onMessage 该注册的都注册了): 补投之前排队进来的消息。
    // 投早了会被 worker 当"没注册 handler"丢掉, 所以必须放在这里而不是 attachWorkerEngine 里。
    if (main)
        main->flushPendingWorkerMessages(m_id);

    // 事件循环: 消息由主引擎直接 postEvent 到本引擎(见 JZJSEngine::customEvent)
    if (m_running)
        exec();

    wtrace("run exec end id=" + std::to_string(m_id));

    // 先把投递落点摘掉(引擎马上要销毁, 不能再被投递), 再拆引擎
    if (main)
        main->detachWorkerEngine(m_id);

    destroyEngine();
    wtrace("run teardown done id=" + std::to_string(m_id));

    // 收尾说明: 本函数返回后, JZThread::start 的线程体会 emit `sigFinished`(无参),
    // 而主引擎在 doSpawnWorker 里已把它 connect 到 JZJSEngine::onWorkerFinished
    // (Auto 连接 + 跨线程 ⇒ 排队到主线程执行) -> 那里做 join + delete + 通知 owner
    // 跑 JS 侧 'exit'。所以这里不需要再自己投任何"结束"事件。
}
