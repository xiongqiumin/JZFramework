#ifndef JZ_JS_WORKER_MODULE_H_
#define JZ_JS_WORKER_MODULE_H_

#include <map>
#include <string>
#include <vector>

#include "JZByteArray.h"
#include "JZJSHandleRef.h"
#include "JZObject.h"
#include "JZJSType.h"
#include "JZJSWorkerThread.h"
#include "quickjs/quickjs.h"

class JZJSEngine;

// =========================================================================
// 设计要点(与 Node worker_threads 的差异)
//
//   1. 每个 worker 一个独立 JSRuntime + 独立 JSContext + **全新空 global**。
//      不继承父线程的任何 JS 全局变量 / 模块级状态; 宿主内建全局
//      (console/process/fs/fetch/__jz.*)由 worker 引擎各自 bootstrap 一遍
//      (重新执行脚本, 不是拷贝父引擎的全局对象 —— 那是跨引擎用 JSValue)。
//   2. 通信只有一条路: 消息(JS_WriteObject/JS_ReadObject 字节) + 按 id 投递。
//      投递直接走 jzApp->postEvent 到"目标引擎的模块", **没有收件箱/暂存队列**。
//   3. 线程表由 JZJSWorkerHub 持有(挂主引擎): 创建/回收都在主线程,
//      worker 里 new Worker 会转到主线程执行。
//   4. worker 内可以再 new Worker(嵌套); 父 worker 退出时级联终止其子 worker。
//
// JS API:
//   __jz.worker = { isMainThread, workerData, selfId(), parentId(), ids(),
//                   childIds(), postTo(id,value), onMessage((fromId,value)=>..), exit() }
//   Worker      = new Worker(entry[,{workerData}]); .id/.exited/.exitCode
//                 .postMessage(v) / .on('exit',cb) / .terminate()
// =========================================================================

// 一个 worker 句柄(JS 侧 `new Worker(...)` 的 C++ 侧)。
// 由 JS GC 拥有(createValue 用 JSRawPointer<JZJSWorker>(obj, false))。
// 只持有 id, 不缓存线程指针 —— 线程由 hub 回收, 句柄不会悬垂。
class JZJSWorker
{
public:
    JZJSWorker();
    ~JZJSWorker();

    JZJSWorkerModule* module;
    int id;
    int parentId;
    std::string entry;
    bool exited;
    int exitCode;
    std::vector<int> exitCbIds;   // 'exit' 回调在模块根数组里的槽位 id
    JZJSHandleRef href;           // 跑着的 worker 是句柄: 创建计一票、退出/terminate/句柄销毁撤销
};

// =========================================================================
// worker 模块: 每个引擎一份(主引擎一份, 每个 worker 引擎各一份)。
//   - 注册 __jz.worker 原子 API 与 Worker 类;
//   - 持有本引擎的回调 GC 根 + 本引擎创建的 worker 句柄表(按 id);
//   - 消息由 hub 直接 postEvent 投到本模块, 在本模块所属线程派发。
// =========================================================================
class JZJSWorkerModule : public JZObject
{
public:
    JZJSWorkerModule(JZObject* parent = nullptr);
    ~JZJSWorkerModule();

    // 必须在 engine 所属线程内调用
    void initEngine(JZJSEngine* engine, int selfId, int parentId, const JZByteArray& workerData);
    void deinitEngine();

    JZJSEngine* engine() const { return m_engine; }
    int selfId() const { return m_selfId; }
    int parentId() const { return m_parentId; }

    // hub 投来的消息 / 退出通知(都在本模块所属线程上被 customEvent 调到)
    void onMessageEvent(int fromId, const JZByteArray& data);
    void onWorkerExit(int workerId, int code);

    // 由 JZJSWorker 析构调用(该句柄被 GC 回收)
    void forgetChild(int id);

    // ---- 回调 GC 根(quickjs 的 GC 会回收"只被 C++ 引用计数持有"的闭包) ----
    int retainCallback(JSValue cb);
    JSValue callback(int id);       // 返回新引用, 调用方负责 Free
    void releaseCallback(int id);

    // ---- 序列化 ----
    bool encode(JSValue value, JZByteArray& out);       // 失败时已设 JS 异常

    // ---- 原子 API(__jz.worker.*) ----
    JSValue jsSelfId();
    JSValue jsParentId();
    JSValue jsIsMainThread();
    JSValue jsIds();
    JSValue jsChildIds();
    JSValue jsPostTo(int targetId, JSValue value);
    JSValue jsOnMessage(JSValue cb);
    JSValue jsExit();

    // ---- Worker 类 ----
    void* createWorker(JSContext* ctx, int argc, JSValueConst* argv);
    JSValue workerPostMessage(JZJSWorker* w, JSValue value);
    JSValue workerOn(JZJSWorker* w, const std::string& event, JSValue cb);
    JSValue workerTerminate(JZJSWorker* w);
    // 句柄 ref 开关(ref()/unref()): 只改"算不算一票让宿主活着"
    JSValue workerRef(JZJSWorker* w, bool refed);
    int workerIdOf(JZJSWorker* w);
    bool workerExitedOf(JZJSWorker* w);
    int workerExitCodeOf(JZJSWorker* w);

private:
    void registNamespace();
    void registWorkerClass();
    JSValue throwError(const std::string& msg);
    JSValue callCallback(int cbId, int argCount, JSValue* args, bool* ok);
    void handleCallbackException();

    JZJSEngine* m_engine;
    int m_selfId;
    int m_parentId;
    int m_typeWorker;          // Worker 类的引擎内类型 id

    JSValue m_workerData;      // worker 入口拿到的 workerData(反序列化后)
    JSValue m_cbRoot;          // 回调 GC 根数组(挂到全局)
    int m_cbId;

    int m_onMessageId;         // onMessage 回调槽位(-1 = 未设置)
    bool m_warnedNoHandler;    // 没挂 onMessage 时只警告一次

    std::map<int, JZJSWorker*> m_children;   // id -> JS 句柄(退出通知据此找句柄)
    bool m_inited;
};

#endif  // !JZ_JS_WORKER_MODULE_H_
