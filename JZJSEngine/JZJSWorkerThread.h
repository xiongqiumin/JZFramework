#ifndef JZ_JS_WORKER_THREAD_H_
#define JZ_JS_WORKER_THREAD_H_
#include <string>
#include <vector>

#include "JZByteArray.h"
#include "JZSemaphore.h"
#include "JZThread.h"

class JZJSEngine;
class JZJSCoreModule;
class JZJSNetModule;
class JZJSWorkerModule;

// worker 事件类型(type >= JZEvent_user 才会进 JZObject::customEvent)
enum
{
    JZJSWorkerEvent_Message = JZEvent_user + 20,   // 一条消息 -> 目标 worker 的引擎
    JZJSWorkerEvent_Exit,                          // worker 已退出 -> owner 引擎(跑 JS 'exit')
};

// 一条跨 worker 消息。
// data 是 JS_WriteObject 得到的字节 —— 唯一能跨引擎的载体:
// 发送方线程/ctx 序列化, 接收方线程/ctx 反序列化。
class JZFRAMEWORK_API JZJSWorkerMessageEvent : public JZEvent
{
public:
    JZJSWorkerMessageEvent(int fromId, const JZByteArray& data);

    int fromId;
    JZByteArray data;
};

// 某个 worker 已退出: 投给 owner 引擎, 由它的 worker 模块跑 JS 侧 'exit' 回调。
//
// 注意: "线程结束, 主引擎去回收" **没有**对应事件 —— JZThread 自带 `sigFinished`
// (JZThread::start 的线程体在 run() 返回后 emit, 见 JZThread.cpp), 主引擎把它
// connect 到 JZJSEngine::onWorkerFinished 即可(Auto 连接判定跨线程 -> 排队到主线程)。
class JZFRAMEWORK_API JZJSWorkerExitEvent : public JZEvent
{
public:
    JZJSWorkerExitEvent(int workerId, int code);

    int workerId;
    int code;
};

// =========================================================================
// worker 线程: 一个 JZThread(自带事件循环) + 线程内独立的 JZJSEngine
//
//   - JSRuntime 只能在建它的线程里用 ⇒ 引擎在本线程 run() 里创建与销毁;
//   - 必须是 JZThread: JZApplication::currentThread() 对未注册线程会 JZ_ASSERT(0)
//     (致命 abort), 而 JZThread::start() 会自动登记;
//   - 线程对象不自己删除(创建它的线程才持有它, 析构也有线程断言):
//     run() 返回后框架 emit `sigFinished`, 主引擎 connect 之后做 join + delete。
//
// 线程表不在这里 —— 它挂在主引擎上(JZJSEngine 的 worker 相关接口)。
// =========================================================================
class JZFRAMEWORK_API JZJSWorkerThread : public JZThread
{
public:
    JZJSWorkerThread(JZObject* parent = nullptr);
    ~JZJSWorkerThread();

    // 创建方在 start() 之前调用(此时线程体还没跑, 直接写字段无需加锁)
    // main: 主引擎 —— 跑起来后要把自己的引擎挂到它的 worker 表上(attachWorkerEngine),
    //       之后消息才投得进来。**必须传**(传空 = 这个 worker 收不到任何消息)。
    void setup(int id, int parentId,
               const std::string& entry, const JZByteArray& workerData,
               const std::string& workDir, const std::vector<std::string>& libraryDirs,
               JZJSEngine* main = nullptr);

    int workerId() const { return m_id; }
    int parentWorkerId() const { return m_parentId; }
    const std::string& entry() const { return m_entry; }

    JZJSEngine* engine() const { return m_engine; }
    JZJSWorkerModule* module() const { return m_module; }

    // 退出码/错误: 0=正常结束(含 JS 自己退出的), 1=引擎初始化失败, 2=入口脚本失败。
    // sigFinished 是无参信号(JZThread 自带), 所以"是否 error"由创建方在这里取 ——
    // 线程在 emit 前写完, 事件队列的锁保证可见性。
    int exitCode() const { return m_exitCode; }
    const std::string& lastError() const { return m_error; }

    // "引擎 + 模块 + __jz 命名空间就绪"(此后投消息一定能命中本 worker)
    bool waitReady(int timeoutMs);
    void markReady();

    // 当前线程的 worker id; -1 = 该线程上没有初始化过引擎
    static int currentWorkerId();
    // 当前线程的 worker id, 未初始化过则按 0(主线程)处理
    static int currentWorkerIdOrMain();
    // 写入当前线程的 worker id(由 JZJSWorkerModule::initEngine 调用)
    static void setCurrentWorkerId(int id);

protected:
    virtual void run() override;

private:
    bool buildEngine();
    void destroyEngine();
    void reportError(const std::string& text);

    int m_id;
    int m_parentId;
    int m_exitCode;
    std::string m_error;
    std::string m_entry;
    JZByteArray m_workerData;
    std::string m_workDir;
    std::vector<std::string> m_libraryDirs;

    JZJSEngine* m_main;        // 主引擎(worker 表在那儿; 见 setup 的 main 参数)
    JZJSEngine* m_engine;
    JZJSWorkerModule* m_module;
    JZJSCoreModule* m_coreModule;
    JZJSNetModule* m_netModule;

    JZSemaphore m_ready;
};

#endif  // !JZ_JS_WORKER_THREAD_H_
