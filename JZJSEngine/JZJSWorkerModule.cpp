#include "JZJSWorkerModule.h"

#include <cstdio>

#include "JZApplication.h"
#include "JZJSBind.h"
#include "JZJSEngine.h"
#include "JZLog.h"

// worker 内部追踪开关: 排查"消息不通/线程不退出"这类问题时改成 1, 往 stderr 打全流程。
// (与 JZJSWorkerThread.cpp 里的同名开关成对使用。)
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

// =========================================================================
// JZJSWorker(句柄)
// =========================================================================
JZJSWorker::JZJSWorker()
{
    module = nullptr;
    id = -1;
    parentId = -1;
    exited = false;
    exitCode = -1;
}

JZJSWorker::~JZJSWorker()
{
    if (!module)
        return;

    // 句柄被 GC 回收: 只"请求"线程退出 —— 回收(wait + delete)由 hub 在它自己的
    // 线程做, 这里绝不能 join(GC 里阻塞会连带卡住整个引擎线程)。
    int workerId = id;
    JZJSWorkerModule* m = module;
    module = nullptr;
    m->forgetChild(workerId);

    JZJSEngine* engine = m->engine();
    if (engine)
        engine->terminateWorker(workerId);
}

// =========================================================================
// JZJSWorkerModule
// =========================================================================
JZJSWorkerModule::JZJSWorkerModule(JZObject* parent)
    : JZObject(parent)
{
    m_engine = nullptr;
    m_selfId = 0;
    m_parentId = -1;
    m_typeWorker = -1;
    m_workerData = JS_UNDEFINED;
    m_cbRoot = JS_UNDEFINED;
    m_cbId = 1;
    m_onMessageId = -1;
    m_warnedNoHandler = false;
    m_inited = false;
}

JZJSWorkerModule::~JZJSWorkerModule()
{
    deinitEngine();
}

void JZJSWorkerModule::initEngine(JZJSEngine* engine, int selfId, int parentId,
                                  const JZByteArray& workerData)
{
    m_engine = engine;
    m_selfId = selfId;
    m_parentId = parentId;
    m_inited = true;

    // 让本线程内的"当前 worker id"查询可用(主线程 0 / worker 自己的 id)
    JZJSWorkerThread::setCurrentWorkerId(selfId);

    JSContext* ctx = engine->context();

    // 回调 GC 根数组: quickjs 的 GC 会回收"只被 C++ 引用计数持有"的闭包
    // (JS 对象 <-> C++ 句柄 <-> 回调 形成循环), 所以回调一律挂在全局可达的数组里。
    m_cbRoot = JS_NewArray(ctx);
    m_engine->setProperty(m_engine->globalObject(), "__jzWorkerCbRoot",
                          JS_DupValue(ctx, m_cbRoot));

    // workerData: 发送方序列化, 这里(接收方线程 + 接收方 ctx)反序列化
    m_workerData = JS_UNDEFINED;
    if (!workerData.isEmpty())
    {
        JSValue v = JS_ReadObject(ctx, (const uint8_t*)workerData.constData(),
                                  (size_t)workerData.size(), JS_READ_OBJ_REFERENCE);
        if (JS_IsException(v))
        {
            handleCallbackException();
            v = JS_UNDEFINED;
        }
        m_workerData = v;
    }

    registNamespace();
    registWorkerClass();

    // 登记到本引擎: 它是消息事件与"worker 退出"事件在这个线程上的落点
    // (主线程 id=0 的引擎也要登记, 否则 worker 投不回主线程)
    m_engine->setWorkerModule(this);
}

void JZJSWorkerModule::deinitEngine()
{
    if (!m_inited)
        return;
    m_inited = false;

    // 1. 先摘掉本引擎的模块指针: 此后进来的消息/退出事件不再转给本模块
    if (m_engine)
        m_engine->setWorkerModule(nullptr);

    // 2. 断开所有句柄对本模块的引用: 本模块马上要 delete, 而句柄由 JS GC 拥有,
    //    JS_FreeRuntime 时才会析构 —— 留指针就是悬垂。
    //    (worker 线程本身的回收由 hub 负责, 本模块不管线程生命周期。)
    for (std::map<int, JZJSWorker*>::iterator it = m_children.begin(); it != m_children.end(); ++it)
        it->second->module = nullptr;
    m_children.clear();

    // 3. 释放 JS 侧资源(必须在 ctx 释放前)
    if (m_engine)
    {
        JSContext* ctx = m_engine->context();
        if (ctx)
        {
            JS_SetPropertyStr(ctx, m_engine->globalObject(), "__jzWorkerCbRoot", JS_UNDEFINED);
            JS_FreeValue(ctx, m_cbRoot);
            JS_FreeValue(ctx, m_workerData);
        }
        m_cbRoot = JS_UNDEFINED;
        m_workerData = JS_UNDEFINED;
        m_onMessageId = -1;
    }

    m_engine = nullptr;
}

// -------------------------------------------------------------------------
// 事件派发
// 注意: 本模块不自己收事件 —— 消息/退出事件都投给"本引擎"(JZObject),
// 由 JZJSEngine::customEvent 转调下面两个方法(这样投递落点天然是引擎)。
// -------------------------------------------------------------------------
void JZJSWorkerModule::onMessageEvent(int fromId, const JZByteArray& data)
{
    if (!m_engine)
        return;

    // 消息只投给"已就绪的模块", 而模块就绪后 run() 才会进事件循环 ——
    // 也就是说入口脚本(通常第一件事就是挂 onMessage)已经跑过了。
    // 这里仍兜一层: 真没挂回调就警告一次并丢弃(不再暂存, 保持"无队列"语义)。
    if (m_onMessageId < 0)
    {
        if (!m_warnedNoHandler)
        {
            m_warnedNoHandler = true;
            LOG_W(std::string("JZJSWorker[") + std::to_string(m_selfId)
                  + "] dropped message: __jz.worker.onMessage is not registered");
        }
        return;
    }

    JSContext* ctx = m_engine->context();
    JSValue value = JS_ReadObject(ctx, (const uint8_t*)data.constData(),
                                  (size_t)data.size(), JS_READ_OBJ_REFERENCE);
    if (JS_IsException(value))
    {
        handleCallbackException();
        return;
    }

    wtrace("dispatch self=" + std::to_string(m_selfId) + " from=" + std::to_string(fromId));

    JSValue args[2];
    args[0] = JS_NewInt32(ctx, fromId);
    args[1] = value;

    bool ok = true;
    callCallback(m_onMessageId, 2, args, &ok);

    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
}

void JZJSWorkerModule::onWorkerExit(int workerId, int code)
{
    JZJSWorker* w = nullptr;
    std::map<int, JZJSWorker*>::iterator it = m_children.find(workerId);
    if (it != m_children.end())
        w = it->second;

    std::vector<int> cbIds;
    if (w)
    {
        w->exited = true;
        w->exitCode = code;
        cbIds = w->exitCbIds;
        w->exitCbIds.clear();
    }

    if (m_engine && !cbIds.empty())
    {
        JSContext* ctx = m_engine->context();
        for (size_t i = 0; i < cbIds.size(); i++)
        {
            JSValue args[1];
            args[0] = JS_NewInt32(ctx, code);
            bool ok = true;
            callCallback(cbIds[i], 1, args, &ok);
            JS_FreeValue(ctx, args[0]);

            releaseCallback(cbIds[i]);     // 'exit' 只跑一次: 清掉根数组槽位
        }
    }
}

void JZJSWorkerModule::forgetChild(int id)
{
    m_children.erase(id);
}

// -------------------------------------------------------------------------
// 回调 GC 根
// -------------------------------------------------------------------------
int JZJSWorkerModule::retainCallback(JSValue cb)
{
    JSContext* ctx = m_engine->context();
    int id = m_cbId++;
    // JS_SetPropertyUint32 会消费传入值的引用 -> 传 dup 的一份
    JS_SetPropertyUint32(ctx, m_cbRoot, (uint32_t)id, JS_DupValue(ctx, cb));
    return id;
}

JSValue JZJSWorkerModule::callback(int id)
{
    if (id < 0 || !m_engine)
        return JS_UNDEFINED;
    return JS_GetPropertyUint32(m_engine->context(), m_cbRoot, (uint32_t)id);
}

void JZJSWorkerModule::releaseCallback(int id)
{
    if (id < 0 || !m_engine)
        return;
    JS_SetPropertyUint32(m_engine->context(), m_cbRoot, (uint32_t)id, JS_UNDEFINED);
}

JSValue JZJSWorkerModule::callCallback(int cbId, int argCount, JSValue* args, bool* ok)
{
    JSContext* ctx = m_engine->context();
    JSValue cb = callback(cbId);
    if (!JS_IsFunction(ctx, cb))
    {
        JS_FreeValue(ctx, cb);
        if (ok)
            *ok = false;
        return JS_UNDEFINED;
    }

    // 走引擎的 callJs: 回调里结算的 promise(续体是微任务)由引擎统一补检查点唤醒
    JSValue r = m_engine->callJs(cb, JS_UNDEFINED, argCount, args);
    JS_FreeValue(ctx, cb);

    if (JS_IsException(r))
    {
        JS_FreeValue(ctx, r);
        handleCallbackException();
        if (ok)
            *ok = false;
        return JS_UNDEFINED;
    }

    if (ok)
        *ok = true;
    return r;
}

void JZJSWorkerModule::handleCallbackException()
{
    if (!m_engine)
        return;
    JSContext* ctx = m_engine->context();
    if (!ctx)
        return;

    JSValue exc = JS_GetException(ctx);
    const char* p = JS_ToCString(ctx, exc);
    std::string text = p ? std::string(p) : std::string("unknown exception");
    if (p)
        JS_FreeCString(ctx, p);
    JS_FreeValue(ctx, exc);

    m_engine->reportUncaughtError(text);
}

// -------------------------------------------------------------------------
// 序列化: 唯一能跨引擎的载体
// -------------------------------------------------------------------------
bool JZJSWorkerModule::encode(JSValue value, JZByteArray& out)
{
    JSContext* ctx = m_engine->context();

    // JS_WRITE_OBJ_REFERENCE: 支持共享引用/循环引用(Node 的 structured clone 亦然)。
    // 不开 JS_WRITE_OBJ_BYTECODE: 函数/闭包不能跨引擎传, 应明确报错。
    size_t len = 0;
    uint8_t* buf = JS_WriteObject(ctx, &len, value, JS_WRITE_OBJ_REFERENCE);
    if (!buf)
        return false;       // 已设 JS 异常(如不可序列化的类型)

    out = JZByteArray((const char*)buf, (int)len);
    js_free(ctx, buf);
    return true;
}

// -------------------------------------------------------------------------
// 原子 API
// -------------------------------------------------------------------------
JSValue JZJSWorkerModule::throwError(const std::string& msg)
{
    JSContext* ctx = m_engine->context();
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, msg.c_str()));
    return JS_Throw(ctx, err);
}

void JZJSWorkerModule::registNamespace()
{
    JSContext* ctx = m_engine->context();
    JSValue global = m_engine->globalObject();

    JSValue jz = m_engine->getProperty(global, "__jz");
    bool jzFromGet = !JS_IsUndefined(jz);
    if (!jzFromGet)
    {
        jz = m_engine->createObject();
        m_engine->setProperty(global, "__jz", jz);
    }

    JSValue ns = m_engine->createObject();
    m_engine->registFunction(ns, "selfId", jsbind::createSelfFunction(this, &JZJSWorkerModule::jsSelfId));
    m_engine->registFunction(ns, "parentId", jsbind::createSelfFunction(this, &JZJSWorkerModule::jsParentId));
    m_engine->registFunction(ns, "isMainThread", jsbind::createSelfFunction(this, &JZJSWorkerModule::jsIsMainThread));
    m_engine->registFunction(ns, "ids", jsbind::createSelfFunction(this, &JZJSWorkerModule::jsIds));
    m_engine->registFunction(ns, "childIds", jsbind::createSelfFunction(this, &JZJSWorkerModule::jsChildIds));
    m_engine->registFunction(ns, "postTo", jsbind::createSelfFunction(this, &JZJSWorkerModule::jsPostTo));
    m_engine->registFunction(ns, "onMessage", jsbind::createSelfFunction(this, &JZJSWorkerModule::jsOnMessage));
    m_engine->registFunction(ns, "exit", jsbind::createSelfFunction(this, &JZJSWorkerModule::jsExit));

    // workerData: 复制一份给属性, 自己那份留给 deinitEngine 释放
    if (!JS_IsUndefined(m_workerData))
        m_engine->setProperty(ns, "workerData", JS_DupValue(ctx, m_workerData));

    m_engine->setProperty(jz, "worker", ns);

    if (jzFromGet)
        JS_FreeValue(ctx, jz);
}

void JZJSWorkerModule::registWorkerClass()
{
    JZJSWorkerModule* self = this;

    jsbind::ClassBind<JZJSWorker> bind_worker("Worker");
    // setCreate 现在的签名是 (JZJSEngine*, argc, argv): 自己造底层对象并**原样返回**(引擎负责包成
    // 基类对应类型的句柄 + 回写宿主句柄), 所以这里把 ctx 从 engine 里取出来再交给 createWorker。
    bind_worker.setCreate([self](JZJSEngine* engine, int argc, JSValueConst* argv) -> void*
                          { return self->createWorker(engine->context(), argc, argv); });

    bind_worker.def("postMessage", [self](JZJSWorker* w, JSValue v) -> JSValue
                                  { return self->workerPostMessage(w, v); });
    bind_worker.def("on", [self](JZJSWorker* w, const std::string& ev, JSValue cb) -> JSValue
                           { return self->workerOn(w, ev, cb); });
    bind_worker.def("terminate", [self](JZJSWorker* w) -> JSValue
                                 { return self->workerTerminate(w); });

    bind_worker.defProperty<int>("id", [self](JZJSWorker* w) { return self->workerIdOf(w); }, nullptr);
    bind_worker.defProperty<bool>("exited", [self](JZJSWorker* w) { return self->workerExitedOf(w); }, nullptr);
    bind_worker.defProperty<int>("exitCode", [self](JZJSWorker* w) { return self->workerExitCodeOf(w); }, nullptr);

    m_engine->registClass(bind_worker.classDefine());
    m_typeWorker = m_engine->ctypeidToType(typeid(JZJSWorker).name());
}

JSValue JZJSWorkerModule::jsSelfId()
{
    return JS_NewInt32(m_engine->context(), m_selfId);
}

JSValue JZJSWorkerModule::jsParentId()
{
    return JS_NewInt32(m_engine->context(), m_parentId);
}

JSValue JZJSWorkerModule::jsIsMainThread()
{
    return JS_NewBool(m_engine->context(), m_selfId == 0 ? 1 : 0);
}

JSValue JZJSWorkerModule::jsIds()
{
    JSContext* ctx = m_engine->context();
    std::vector<int> ids = m_engine->workerIds();

    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < ids.size(); i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewInt32(ctx, ids[i]));
    return arr;
}

JSValue JZJSWorkerModule::jsChildIds()
{
    JSContext* ctx = m_engine->context();
    std::vector<int> ids = m_engine->workerChildIds(m_selfId);

    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < ids.size(); i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewInt32(ctx, ids[i]));
    return arr;
}

JSValue JZJSWorkerModule::jsPostTo(int targetId, JSValue value)
{
    JZByteArray data;
    if (!encode(value, data))
        return JS_EXCEPTION;

    if (!m_engine->postToWorker(targetId, m_selfId, data))
    {
        wtrace("postTo fail self=" + std::to_string(m_selfId) + " target=" + std::to_string(targetId));
        return throwError("postTo: worker " + std::to_string(targetId) + " not found or not ready");
    }
    return JS_UNDEFINED;
}

JSValue JZJSWorkerModule::jsOnMessage(JSValue cb)
{
    JSContext* ctx = m_engine->context();
    if (!JS_IsFunction(ctx, cb))
        return throwError("onMessage: a function is required");

    if (m_onMessageId >= 0)
        releaseCallback(m_onMessageId);
    m_onMessageId = retainCallback(cb);
    m_warnedNoHandler = false;
    return JS_UNDEFINED;
}

JSValue JZJSWorkerModule::jsExit()
{
    if (m_selfId == 0)
        return throwError("exit: cannot end the main thread this way");

    m_engine->terminateWorker(m_selfId);
    return JS_UNDEFINED;
}

// -------------------------------------------------------------------------
// Worker 类
// -------------------------------------------------------------------------
void* JZJSWorkerModule::createWorker(JSContext* ctx, int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsString(argv[0]))
    {
        JS_ThrowTypeError(ctx, "new Worker(entry[, options]): entry must be a script path (string)");
        return nullptr;
    }

    const char* p = JS_ToCString(ctx, argv[0]);
    std::string entry = p ? std::string(p) : std::string();
    if (p)
        JS_FreeCString(ctx, p);

    if (entry.empty())
    {
        JS_ThrowTypeError(ctx, "new Worker(entry[, options]): entry must not be empty");
        return nullptr;
    }

    // workerData: 在创建方线程/ctx 里序列化, 传字节过去
    JZByteArray data;
    if (argc >= 2 && JS_IsObject(argv[1]))
    {
        JSValue wd = JS_GetPropertyStr(ctx, argv[1], "workerData");
        if (!JS_IsUndefined(wd))
        {
            bool ok = encode(wd, data);
            JS_FreeValue(ctx, wd);
            if (!ok)
                return nullptr;     // encode 已设 JS 异常
        }
        else
        {
            JS_FreeValue(ctx, wd);
        }
    }

    // 统一交给主引擎的 worker 表(worker 里 new Worker 也走这条:
    // 引擎会把创建线程对象的活转到主线程, 再同步拿回 id)
    // owner = parent = 创建者: exit 事件回到本引擎(句柄在这里), 级联也按此收敛
    int id = m_engine->spawnWorker(entry, data, m_selfId, m_selfId,
                                   m_engine->workDirPath(), m_engine->libraryDirs());
    if (id < 0)
    {
        JS_ThrowInternalError(ctx, "new Worker: failed to start worker '%s'", entry.c_str());
        return nullptr;
    }

    JZJSWorker* w = new JZJSWorker();
    w->module = this;
    w->id = id;
    w->parentId = m_selfId;
    w->entry = entry;
    w->exited = false;
    w->exitCode = -1;
    m_children[id] = w;

    wtrace("createWorker owner=" + std::to_string(m_selfId) + " id=" + std::to_string(id)
           + " entry=" + entry + " dataBytes=" + std::to_string(data.size()));

    return w;
}

JSValue JZJSWorkerModule::workerPostMessage(JZJSWorker* w, JSValue value)
{
    if (!w)
        return throwError("postMessage: invalid Worker handle");
    if (w->exited)
        return throwError("postMessage: worker " + std::to_string(w->id) + " already exited");

    JZByteArray data;
    if (!encode(value, data))
        return JS_EXCEPTION;

    if (!m_engine->postToWorker(w->id, m_selfId, data))
        return throwError("postMessage: worker " + std::to_string(w->id) + " not found or not ready");

    return JS_UNDEFINED;
}

JSValue JZJSWorkerModule::workerOn(JZJSWorker* w, const std::string& event, JSValue cb)
{
    JSContext* ctx = m_engine->context();
    if (!w)
        return throwError("on: invalid Worker handle");
    if (event != "exit")
    {
        // v1 只做 'exit'; 收消息走全局 __jz.worker.onMessage((fromId, value) => ...)
        return throwError("on: v1 only supports the 'exit' event; use __jz.worker.onMessage for messages");
    }
    if (!JS_IsFunction(ctx, cb))
        return throwError("on('exit', cb): the second argument must be a function");

    w->exitCbIds.push_back(retainCallback(cb));
    return JS_UNDEFINED;
}

JSValue JZJSWorkerModule::workerTerminate(JZJSWorker* w)
{
    if (!w)
        return throwError("terminate: invalid Worker handle");

    m_engine->terminateWorker(w->id);
    return JS_UNDEFINED;
}

int JZJSWorkerModule::workerIdOf(JZJSWorker* w)
{
    return w ? w->id : -1;
}

bool JZJSWorkerModule::workerExitedOf(JZJSWorker* w)
{
    return w ? w->exited : true;
}

int JZJSWorkerModule::workerExitCodeOf(JZJSWorker* w)
{
    return w ? w->exitCode : -1;
}
