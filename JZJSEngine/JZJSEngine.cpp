#include "JZJSEngine.h"

#include <assert.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

#include "JZJSBind.h"
#include "JZApplication.h"
#include "JZDateTime.h"
#include "JZDir.h"
#include "JZElapsedTimer.h"
#include "JZFile.h"
#include "JZFileInfo.h"
#include "JZLog.h"
#include "JZPlatform.h"
#include "JZStd.h"

using namespace jsbind;
using namespace std;

class JZJSEngineInternalFunction
{
public:
    static JSValue js_sleep(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
    {
        if (argc != 1)
        {
            return JS_ThrowTypeError(ctx, "参数错误: 需要函数和时间间隔");
        }

        int interval_ms;
        if (JS_ToInt32(ctx, &interval_ms, argv[0]) != 0 || interval_ms <= 0)
        {
            return JS_ThrowTypeError(ctx, "时间间隔必须是正整数");
        }

        JZThread::msleep(interval_ms);
        return JS_UNDEFINED;
    }

    static JSValue js_set_interval(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
    {
        if (argc < 2 || !JS_IsFunction(ctx, argv[0]))
        {
            return JS_ThrowTypeError(ctx, "参数错误: 需要函数和时间间隔");
        }

        int interval_ms;
        if (JS_ToInt32(ctx, &interval_ms, argv[1]) != 0 || interval_ms <= 0)
        {
            return JS_ThrowTypeError(ctx, "时间间隔必须是正整数");
        }

        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        int id = 0;
        if (magic == 0)
            engine->setInterval(argv[0], interval_ms, false);
        else
            engine->setInterval(argv[0], interval_ms, true);

        return JS_NewInt32(ctx, id);
    }

    static JSValue js_clear_interval(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
    {
        if (argc < 1)
        {
            return JS_ThrowTypeError(ctx, "参数错误: 时间间隔");
        }

        int id;
        if (JS_ToInt32(ctx, &id, argv[0]) != 0)
        {
            return JS_ThrowTypeError(ctx, "时间间隔必须是正整数");
        }

        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        engine->clearInterval(id);

        return JS_UNDEFINED;
    }

    static JSValue js_is_interruption(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        bool flag = engine->isInterruption();
        return JS_NewBool(ctx, flag);
    }

    static JSValue js_console_log(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        engine->print(this_val, argc, argv);
        return JS_UNDEFINED;
    }

    static JSValue js_fs_readString(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);

        if (argc != 1)
            return engine->makeParamNumError(1);

        string path;
        if (!js_to_value(ctx, argv[0], path))
            return engine->makeTypeError(0, JSType_string);

        path = engine->fullPath(path);

        JZFile file;
        if (!file.open(path.c_str(), "rb"))
            return engine->makeOpenFileError(path);

        JZByteArray result = file.readAll();
        file.close();
        return JS_NewString(ctx, result.data());
    }

    static JSValue js_cfunction(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        return engine->call(magic, argc, argv);
    }

    static JSValue js_async_cfunction(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        return engine->callAsync(magic, argc, argv);
    }

    static JSValue js_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv, int magic)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        return engine->createValue(magic);
    }

    static void js_deconstructor(JSRuntime* rt, JSValue val)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetRuntimeOpaque(rt);
        engine->deconstructor(val);
    }

    static JSValue js_member_function(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        std::vector<JSValue> args;
        args.push_back(this_val);
        for (int i = 0; i < argc; i++)
            args.push_back(argv[i]);
        return engine->call(magic, (int)args.size(), args.data());
    }

    static JSValue js_property_get(JSContext* ctx, JSValue this_val, int magic)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);

        std::vector<JSValue> args;
        args.push_back(this_val);
        return engine->call(magic, (int)args.size(), args.data());
    }

    static JSValue js_property_set(JSContext* ctx, JSValue this_val, JSValue val, int magic)
    {
        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);

        std::vector<JSValue> args;
        args.push_back(this_val);
        args.push_back(val);
        return engine->call(magic + 1, (int)args.size(), args.data());
    }

    static void js_promise_reject(JSContext* ctx, JSValueConst promise, JSValueConst reason, bool is_handled,
                                  void* opaque)
    {
        if (is_handled)
            return;

        JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        engine->unHandlePromiseReject(promise, reason);
    }
};
using JSInter = JZJSEngineInternalFunction;

// JZJSValuePtr
JZJSValuePtr::JZJSValuePtr()
{
    m_value = JS_UNDEFINED;
}

JZJSValuePtr::JZJSValuePtr(JSContext* ctx, JSValue value)
{
    m_value = value;
    m_guard = std::shared_ptr<void>((void*)nullptr, [ctx, value](void*) { JS_FreeValue(ctx, value); });
}

JZJSValuePtr::~JZJSValuePtr()
{
}

JSValue JZJSValuePtr::get() const
{
    return m_value;
}

// JZJSEngine
JZJSEngine::TypeInfo::TypeInfo()
{
    type = JSType_none;
    valueType = JSType_none;
    jsType = JS_TAG_UNDEFINED;
}

JZJSEngine::TimerTask::TimerTask()
{
    runOnce = false;
    callback = JS_UNDEFINED;
    interval = 0;
    next_run_time = 0;
    id = 0;
}

JZJSEngine::PromiseTask::PromiseTask()
{
    ctx = nullptr;
    resolve = JS_UNDEFINED;
    reject = JS_UNDEFINED;
}

JZJSEngine::PromiseTask::~PromiseTask()
{
    if (ctx)
    {
        JS_FreeValue(ctx, resolve);
    }
}

JZJSEngine::JZJSEngine(JZObject* parent)
    : JZObject(parent), sigStartResult(this), sigStatus(this), sigLog(this), sigError(this)
{
    m_ctx = nullptr;
    m_rt = nullptr;
    m_global = JS_UNDEFINED;

    m_timeTaskId = 0;
    m_promiseTaskId = 0;
    m_objectId = JSType_objectUser;

    m_timer = INVAILD_TIMER_ID;
    m_interrupt = false;
    m_status = Engine_none;
}

JZJSEngine::~JZJSEngine()
{
}

void JZJSEngine::init()
{
    assert(!m_ctx);
    initJSEnv();
}

void JZJSEngine::deinit()
{
    deinitJSEnv();
}

void JZJSEngine::initJSEnv()
{
    initTypeMap();

    m_objectId = JSType_objectUser;
    m_timeTaskId = 0;
    m_promiseTaskId = 0;
    m_interrupt = false;

    m_rt = JS_NewRuntime();
    m_ctx = JS_NewContext(m_rt);
    m_global = JS_GetGlobalObject(m_ctx);

    JS_SetHostPromiseRejectionTracker(m_rt, &JSInter::js_promise_reject, nullptr);

    registInternal();
    registVector<int>();
    registVector<double>();
    registVector<string>();

    registConsole();
}

void JZJSEngine::deinitJSEnv()
{
    stopEngineTimer();
    if (m_rt)
    {
        for (int i = 0; i < (int)m_timeTasks.size(); i++)
        {
            JS_FreeValue(m_ctx, m_timeTasks[i].callback);
        }
        m_timeTasks.clear();
        m_promiseTask.clear();

        JS_FreeValue(m_ctx, m_global);
        JS_FreeContext(m_ctx);
        JS_FreeRuntime(m_rt);
        setStatus(Engine_none);
    }
    m_global = JS_NULL;
    m_ctx = nullptr;
    m_rt = nullptr;

    m_workDir.clear();
    m_promiseError.clear();
    m_runtimeError.clear();
    m_funcList.clear();
    m_typeMap.clear();
    m_cTypeMap.clear();
    m_jsTypeMap.clear();
}

void JZJSEngine::setInterruption(bool flag)
{
    m_interrupt = flag;
}

bool JZJSEngine::isInterruption() const
{
    return m_interrupt;
}

void JZJSEngine::wait(int delay)
{
    JZElapsedTimer t;
    t.start();
    while (t.elapsed() < delay)
    {
        if (isInterruption())
            return;

        JZThread::msleep(20);
    }
}

void JZJSEngine::initTypeMap()
{
    registBase("JSValue", JSType_oriValue, -100, typeid(JSValue).name());

    registBase("bool", JSType_bool, JS_TAG_BOOL, typeid(bool).name());
    registBase("int", JSType_int, JS_TAG_INT, typeid(int).name());
    registBase("double", JSType_double, JS_TAG_FLOAT64, typeid(double).name());
    registBase("string", JSType_string, JS_TAG_STRING, typeid(string).name());
    registBase("any", JSType_jsonValue, JS_TAG_OBJECT, typeid(Json::Value).name());
}

void JZJSEngine::registInternal()
{
    string init_code = R"(
function __printObject__(obj, seen = new WeakSet()) {
    // 处理循环引用
    if (seen.has(obj)) {
        return "[Circular]";
    }

    let result;
    // 处理 null（typeof null 是 'object'，需要单独判断）
    if (obj === null) {
        result = "null";
    }
    // 处理 Error 对象
    else if (obj instanceof Error) {
        result = `${obj.name || 'UnknownError'}: ${obj.message}`;
        // 可选：保留错误堆栈（如果需要）
        // if (obj.stack) result += `\n${obj.stack}`;
    }
    // 处理数组
    else if (Array.isArray(obj)) {
        seen.add(obj); // 标记数组已遍历，防止循环引用
        result = "[";
        // 使用 let 替代 var，避免变量提升问题
        for (let i = 0; i < obj.length; i++) {
            result += __printObject__(obj[i], seen);
            if (i !== obj.length - 1) {
                result += ", ";
            }
        }
        result += "]";
        seen.delete(obj); // 遍历完成后移除标记
    }
    // 处理普通对象
    else if (typeof obj === 'object') {
        seen.add(obj); // 标记对象已遍历，防止循环引用
        const strList = []; // 使用 const 替代 var，符合现代 JS 规范
        for (const key in obj) {
            strList.push(`${key}: ${__printObject__(obj[key], seen)}`);
        }
        result = `{${strList.join(", ")}}`;
        
        // 处理自定义原型对象的类名
        const proto = Object.getPrototypeOf(obj);
        if (proto !== null && proto !== Object.prototype) {
            const className = proto.constructor?.name;
            if (className) {
                result = `${className}${result}`;
            }
        }
        seen.delete(obj); // 遍历完成后移除标记
    }
    // 处理字符串（添加引号包裹）
    else if (typeof obj === 'string') {
        result = JSON.stringify(obj);
    }
    // 处理其他类型（数字、布尔、undefined 等）
    else {
        result = String(obj);
    }

    return result;
}
)";

    bool ret_init = loadScript(init_code);
    assert(ret_init);

    JS_SetRuntimeOpaque(m_rt, this);
    JS_SetContextOpaque(m_ctx, this);

    JSValue sleep_func = JS_NewCFunction(m_ctx, JSInter::js_sleep, "sleep", 1);
    JS_SetPropertyStr(m_ctx, m_global, "sleep", sleep_func);

    JSValue set_interval =
        JS_NewCFunctionMagic(m_ctx, JSInter::js_set_interval, "setInterval", 2, JS_CFUNC_generic_magic, 0);
    JS_SetPropertyStr(m_ctx, m_global, "setInterval", set_interval);

    JSValue set_timeout =
        JS_NewCFunctionMagic(m_ctx, JSInter::js_set_interval, "setInterval", 2, JS_CFUNC_generic_magic, 1);
    JS_SetPropertyStr(m_ctx, m_global, "setTimeout", set_timeout);

    JSValue clear_interval =
        JS_NewCFunctionMagic(m_ctx, JSInter::js_clear_interval, "clearInterval", 2, JS_CFUNC_generic_magic, 0);
    JS_SetPropertyStr(m_ctx, m_global, "clearInterval", clear_interval);

    JSValue clear_timeout =
        JS_NewCFunctionMagic(m_ctx, JSInter::js_clear_interval, "clearTimeout", 2, JS_CFUNC_generic_magic, 1);
    JS_SetPropertyStr(m_ctx, m_global, "clearTimeout", clear_timeout);

    // engine
    JSValue engine = JS_NewObject(m_ctx);

    JSValue interrup_func = JS_NewCFunction(m_ctx, JSInter::js_is_interruption, "isInterruption", 2);
    JS_SetPropertyStr(m_ctx, engine, "isInterruption", interrup_func);
    JS_SetPropertyStr(m_ctx, m_global, "engine", engine);
}

void JZJSEngine::registConsole()
{
    JSValue console = JS_NewObject(m_ctx);

    // console
    JSValue log_func = JS_NewCFunction(m_ctx, JSInter::js_console_log, "log", 1);
    JSValue error_func = JS_NewCFunction(m_ctx, JSInter::js_console_log, "error", 1);
    JS_SetPropertyStr(m_ctx, console, "log", log_func);
    JS_SetPropertyStr(m_ctx, console, "error", error_func);
    JS_SetPropertyStr(m_ctx, m_global, "console", console);

    // process
    JSValue process = JS_NewObject(m_ctx);
    registFunction(process, "cwd", jsbind::createFunction([this]() -> string { return m_workDir; }));
    JS_SetPropertyStr(m_ctx, m_global, "process", process);

    // fs
    JSValue read_str_func = JS_NewCFunction(m_ctx, JSInter::js_fs_readString, "readString", 1);

    JSValue fs = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, fs, "readString", read_str_func);
    JS_SetPropertyStr(m_ctx, m_global, "fs", fs);
}

std::string JZJSEngine::workDir()
{
    return m_workDir;
}

std::string JZJSEngine::fullPath(std::string& path)
{
    JZDir dir(m_workDir);
    return dir.absoluteFilePath(path);
}

int JZJSEngine::status()
{
    return m_status;
}

void JZJSEngine::setStatus(JZJSEngineStatus status)
{
    if (m_status != status)
    {
        assert((m_status == Engine_none && status == Engine_running) ||
               (m_status == Engine_running &&
                (status == Engine_pause || status == Engine_error || status == Engine_none)) ||
               (m_status == Engine_pause && (status == Engine_running || status == Engine_none)) ||
               (m_status == Engine_error && (status == Engine_running || status == Engine_none)));

        LOG_FMT_I("engine status changed %d -> %d", m_status, status);

        m_status = status;
        sigStatus.emit(status);
    }
}

void JZJSEngine::start()
{
    startEngineTimer();
    setStatus(Engine_running);
}

void JZJSEngine::stop()
{
    if (!m_ctx)
        return;

    stopEngineTimer();
    setStatus(Engine_none);
}

void JZJSEngine::pause()
{
    assert(m_ctx);
    stopEngineTimer();
    setStatus(Engine_pause);
}

void JZJSEngine::resetUncaughtError()
{
    JS_ResetUncatchableError(m_ctx);
    setStatus(Engine_running);
}

void JZJSEngine::resume()
{
    assert(m_ctx && m_status == Engine_pause);
    startEngineTimer();
    setStatus(Engine_running);
}

void JZJSEngine::startEngineTimer()
{
    if (m_timer == INVAILD_TIMER_ID)
    {
        m_timer = startTimer(10);
        LOG_I("startEngineTimer");
    }
}

void JZJSEngine::stopEngineTimer()
{
    if (m_timer != INVAILD_TIMER_ID)
    {
        killTimer(m_timer);
        m_timer = INVAILD_TIMER_ID;

        LOG_I("stopEngineTimer");
    }
}

bool JZJSEngine::load(const std::string& filepath)
{
    JZFile file;
    if (!file.open(filepath, "rt"))
    {
        LOG_E("open path " + filepath + " failed");
        sigLog.emit("open path " + filepath + " failed");
        return false;
    }

    JZByteArray js_bytecode = file.readAll();
    file.close();

    string js_code = str_fromUtf8(js_bytecode);
    return loadScript(js_code);
}

bool JZJSEngine::loadScript(const std::string& js_code)
{
    JSValue result = JS_Eval(m_ctx, js_code.data(), js_code.size(), "script", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result))
    {
        JSValue exc = JS_GetException(m_ctx);
        std::string runtimeError = getExceptionDescript‌(exc);
        JS_FreeValue(m_ctx, exc);

        JS_FreeValue(m_ctx, result);
        return false;
    }
    JS_FreeValue(m_ctx, result);
    return true;
}

void JZJSEngine::timerEvent(JZTimerEvent* event)
{
    execOnce();
}

void JZJSEngine::customEvent(JZEvent* event)
{
}

int64_t JZJSEngine::currentMs()
{
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

std::string JZJSEngine::getPropertyStr(JSValue var, std::string name)
{
    std::string ret;

    JSValue prop = JS_GetPropertyStr(m_ctx, var, name.c_str());
    const char* msg = JS_ToCString(m_ctx, prop);
    ret = msg;
    JS_FreeCString(m_ctx, msg);

    return ret;
}

bool JZJSEngine::isException(JSValue value)
{
    return JS_IsException(value);
}

std::string JZJSEngine::getExceptionDescript‌(JSValue exc)
{
    std::string msg = getPropertyStr(exc, "message");
    std::string stack = getPropertyStr(exc, "stack");
    std::string error = std::string(msg) + "\n" + stack;
    return error;
}

void JZJSEngine::unHandleException()
{
    JSValue exc = JS_GetException(m_ctx);
    std::string runtimeError = getExceptionDescript‌(exc);
    JS_FreeValue(m_ctx, exc);

    onExceptionOccurred(runtimeError);
}

void JZJSEngine::unHandlePromiseReject(JSValueConst promise, JSValueConst reason)
{
    if (JS_IsException(reason))
    {
        JSValue exc = JS_GetException(m_ctx);
        std::string runtimeError = getExceptionDescript‌(exc);
        JS_FreeValue(m_ctx, exc);

        m_promiseError = "unHandle Exception";
    }
    else if (JS_IsError(m_ctx, reason))
        m_promiseError = getExceptionDescript‌(reason);
    else
    {
        m_promiseError = printObject(reason);
    }
}

JZJSEngine::PromiseTask* JZJSEngine::getPromiseTask(int handle)
{
    auto it = m_promiseTask.find(handle);
    if (it == m_promiseTask.end())
        return nullptr;

    return it->second.get();
}

void JZJSEngine::updateFunction(shared_ptr<CFunction> func)
{
    for (int i = 0; i < (int)func->params.size(); i++)
    {
        int type = ctypeidToType(func->params[i].ctypeid);
        assert(type != JSType_none);
        func->params[i].type = type;
    }

    if (!func->result.ctypeid.empty())
    {
        int ret_type = ctypeidToType(func->result.ctypeid);
        assert(ret_type != JSType_none);
        func->result.type = ret_type;
    }
}

void JZJSEngine::registFunction(JSValue this_val, std::string name, std::shared_ptr<CFunction> func)
{
    updateFunction(func);

    auto js_func = JS_NewCFunctionMagic(m_ctx, JSInter::js_cfunction, name.c_str(), (int)func->params.size(),
                                        JS_CFUNC_generic_magic, (int)m_funcList.size());
    JS_SetPropertyStr(m_ctx, this_val, name.c_str(), js_func);

    m_funcList.push_back(func);
}

void JZJSEngine::registAsyncFunction(JSValue this_val, string name, shared_ptr<CFunction> func)
{
    updateFunction(func);
    JZ_ASSERT(func->params.size() > 0 && func->params[0].type == JSType_int);

    auto js_func = JS_NewCFunctionMagic(m_ctx, JSInter::js_async_cfunction, name.c_str(), (int)func->params.size() - 1,
                                        JS_CFUNC_generic_magic, (int)m_funcList.size());
    JS_SetPropertyStr(m_ctx, this_val, name.c_str(), js_func);

    m_funcList.push_back(func);
}

void JZJSEngine::registType(TypeInfo type_info)
{
    assert(m_typeMap.find(type_info.type) == m_typeMap.end());
    m_typeMap[type_info.type] = type_info;

    m_jsTypeMap[type_info.jsType] = type_info.type;
    m_cTypeMap[type_info.ctypeId] = type_info.type;
}

void JZJSEngine::registBase(string name, int type, int js_type, string ctypeid)
{
    TypeInfo type_info;
    type_info.name = name;
    type_info.type = type;
    type_info.jsType = js_type;
    type_info.ctypeId = ctypeid;
    registType(type_info);
}

void JZJSEngine::registClass(const CClassDefine& define, int define_id)
{
    JSClassDef js_define = {};
    js_define.class_name = define.name.c_str();
    js_define.finalizer = JSInter::js_deconstructor;

    JSValue js_proto, js_class;

    int object_id;
    if (define_id != JSType_none)
        object_id = define_id;
    else
        object_id = m_objectId++;

    JSClassID js_class_id = object_id;
    JS_NewClass(m_rt, js_class_id, &js_define);

    auto funcList = std::make_shared<vector<JSCFunctionListEntry>>();

    TypeInfo info;
    info.name = define.name;
    info.type = object_id;
    info.ctypeId = define.ctypeid;
    info.jsType = js_class_id;
    info.classDefine = define;
    info.funcList = funcList;
    registType(info);

    for (int i = 0; i < (int)define.propertys.size(); i++)
    {
        auto& prop = define.propertys[i];
        updateFunction(prop.read);
        updateFunction(prop.write);

        int magic = (int)m_funcList.size();

        JSCFunctionListEntry entry = {};
        entry.name = prop.name.c_str();
        entry.def_type = JS_DEF_CGETSET_MAGIC;
        entry.magic = magic;

        m_funcList.push_back(prop.read);
        entry.u.getset.get.getter_magic = JSInter::js_property_get;

        if (!prop.write)
            entry.prop_flags = JS_PROP_ENUMERABLE;
        else
        {
            entry.prop_flags = JS_PROP_WRITABLE | JS_PROP_ENUMERABLE;
            m_funcList.push_back(prop.write);
            entry.u.getset.set.setter_magic = JSInter::js_property_set;
        }

        funcList->push_back(entry);
    }
    for (int i = 0; i < (int)define.functions.size(); i++)
    {
        auto& func = define.functions[i];
        updateFunction(func);

        int magic = (int)m_funcList.size();
        m_funcList.push_back(func);

        JSCFunctionListEntry entry = {};
        entry.name = func->name.c_str();
        entry.def_type = JS_DEF_CFUNC;
        entry.prop_flags = JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE;
        entry.magic = magic;
        entry.u.func.length = (uint8_t)func->params.size();
        entry.u.func.cproto = JS_CFUNC_generic_magic;
        entry.u.func.cfunc.generic_magic = JSInter::js_member_function;

        funcList->push_back(entry);
    }
    js_proto = JS_NewObject(m_ctx);
    JS_SetPropertyFunctionList(m_ctx, js_proto, funcList->data(), (int)funcList->size());

    js_class = JS_NewCFunctionMagic(m_ctx, JSInter::js_constructor, define.name.c_str(), 0, JS_CFUNC_constructor_magic,
                                    object_id);
    JS_SetConstructor(m_ctx, js_class, js_proto);
    JS_SetClassProto(m_ctx, js_class_id, js_proto);

    JS_SetPropertyStr(m_ctx, m_global, define.name.c_str(), js_class);
}

int JZJSEngine::setInterval(JSValue callback, int64_t interval, bool once)
{
    TimerTask task;
    task.runOnce = once;
    task.callback = JS_DupValue(m_ctx, callback);
    task.interval = interval;
    task.next_run_time = currentMs() + interval;
    task.id = m_timeTaskId++;

    m_timeTasks.push_back(task);
    return task.id;
}

void JZJSEngine::clearInterval(int id)
{
    int idx = -1;
    for (int i = 0; i < m_timeTasks.size(); i++)
    {
        if (m_timeTasks[i].id == id)
        {
            idx = i;
            break;
        }
    }
    if (idx == -1)
        return;

    JS_FreeValue(m_ctx, m_timeTasks[idx].callback);
    vec_removeAt(m_timeTasks, idx);
}

string JZJSEngine::typeName(int type)
{
    auto it = m_typeMap.find(type);
    if (it == m_typeMap.end())
        return "none";

    return it->second.name;
}

void JZJSEngine::print(JSValueConst this_val, int argc, JSValueConst* argv)
{
    std::string log;
    for (int i = 0; i < argc; i++)
    {
        string text = printObject(argv[i]);
        log += text;
        if (i < argc - 1)
        {
            log += " ";
        }
    }
    sigLog.emit(log);
}

string JZJSEngine::printObject(JSValue value)
{
    JSValue print_func = JS_GetPropertyStr(m_ctx, m_global, "__printObject__");
    JSValue argv[] = {value};
    JSValue result = JS_Call(m_ctx, print_func, JS_NULL, 1, argv);
    string str = ::fromJSValue<string>(m_ctx, result);
    JS_FreeValue(m_ctx, print_func);
    JS_FreeValue(m_ctx, result);

    return str;
}

bool JZJSEngine::isVectorType(int type)
{
    auto it = m_typeMap.find(type);
    assert(it != m_typeMap.end());

    return (it->second.valueType != JSType_none);
}

int JZJSEngine::ctypeidToType(string ctypeid)
{
    if (ctypeid.size() == 0)
        return JSType_none;

    if (ctypeid.back() == '*')
        ctypeid = ctypeid.substr(0, ctypeid.size() - 1);

    auto it = m_cTypeMap.find(ctypeid);
    if (it == m_cTypeMap.end())
        return JSType_none;

    return it->second;
}

int JZJSEngine::jsTypeToType(int js_type)
{
    auto it = m_jsTypeMap.find(js_type);
    if (it == m_jsTypeMap.end())
        return JSType_none;

    return it->second;
}

int JZJSEngine::typeToJSType(int type)
{
    auto it = m_jsTypeMap.begin();
    while (it != m_jsTypeMap.end())
    {
        if (it->second == type)
            return it->first;

        it++;
    }
    return JS_TAG_UNDEFINED;
}

int JZJSEngine::jsValueType(JSValue v)
{
    int js_type = JS_VALUE_GET_TAG(v);
    if (js_type == JS_TAG_OBJECT)
    {
        js_type = JS_GetClassID(v);
        if (js_type == 1)  // JS_CLASS_OBJECT
            return JSType_jsonValue;
    }
    return jsTypeToType(js_type);
}

bool JZJSEngine::canConvert(int from_type, int to_type)
{
    if (from_type == to_type)
        return true;

    if (from_type >= JSType_bool && from_type <= JSType_double && to_type >= JSType_bool && to_type <= JSType_double)
        return true;

    return false;
}

bool JZJSEngine::checkType(JSValue value, int type)
{
    if (isVectorType(type))
    {
        if (!JS_IsArray(value))
            return false;

        int sub_type = m_typeMap[type].valueType;
        int64_t length = 0;
        JS_GetLength(m_ctx, value, &length);
        for (int i = 0; i < length; i++)
        {
            JSValue sub = JS_GetPropertyInt64(m_ctx, value, i);
            bool ret = checkType(sub, sub_type);
            JS_FreeValue(m_ctx, sub);
            if (!ret)
                return false;
        }

        return true;
    }
    int js_type = jsValueType(value);
    if (js_type >= JSType_object && type == JSType_jsonValue)
        return true;

    return canConvert(js_type, type);
}

JSValue JZJSEngine::makeParamNumError(int num)
{
    return JS_ThrowTypeError(m_ctx, "需要%d个参数", num);
}

string JZJSEngine::makeTypeErrorText(int i, int type)
{
    return str_asprintf("输入%d需要类型%s", i, typeName(type).c_str());
}

string JZJSEngine::makeGetPropErrorText(std::string prop)
{
    return str_asprintf("读取属性%s失败", prop.c_str());
}

JSValue JZJSEngine::makeTypeError(int i, int type)
{
    return JS_ThrowTypeError(m_ctx, "输入%d需要类型%s", i, typeName(type).c_str());
}

JSValue JZJSEngine::makeOpenFileError(string file)
{
    return JS_ThrowPlainError(m_ctx, "打开文件\"%s\"失败", file.c_str());
}

JSValue JZJSEngine::globalObject()
{
    return m_global;
}

JSValue JZJSEngine::createObject()
{
    JSValue obj = JS_NewObject(m_ctx);
    return obj;
}

JSValue JZJSEngine::createValue(int type)
{
    if (type == JSType_bool)
    {
        return JS_NewBool(m_ctx, 0);
    }
    else if (type == JSType_int)
    {
        return JS_NewInt32(m_ctx, 0);
    }
    else if (type == JSType_double)
    {
        return JS_NewNumber(m_ctx, 0.0);
    }
    else if (type == JSType_string)
    {
        return JS_NewString(m_ctx, "");
    }
    else
    {
        JSClassID js_class_id = typeToJSType(type);

        JSValue proto = JS_GetClassProto(m_ctx, js_class_id);

        void* ptr = constructor(type);
        JSValue obj = JS_NewObjectProtoClass(m_ctx, proto, js_class_id);
        JS_SetOpaque(obj, ptr);
        assert(JS_GetAnyOpaque(obj, &js_class_id) == ptr);

        JS_FreeValue(m_ctx, proto);

        return obj;
    }
}

JSValue JZJSEngine::createValue(int type, void* ptr)
{
    JSClassID js_class_id = typeToJSType(type);

    JSValue proto = JS_GetClassProto(m_ctx, js_class_id);

    JSValue obj = JS_NewObjectProtoClass(m_ctx, proto, js_class_id);
    JS_SetOpaque(obj, ptr);
    assert(JS_GetAnyOpaque(obj, &js_class_id) == ptr);

    JS_FreeValue(m_ctx, proto);

    return obj;
}

void JZJSEngine::freeValue(JSValue value)
{
    JS_FreeValue(m_ctx, value);
}

JSValue JZJSEngine::dupValue(JSValue value)
{
    return JS_DupValue(m_ctx, value);
}

JSValue JZJSEngine::createResolvedPromise(JSValue jsValue)
{
    // 1. 创建 Promise + resolve/reject 函数
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(m_ctx, resolving_funcs);

    JS_Call(m_ctx, resolving_funcs[0], JS_UNDEFINED, 1, &jsValue);

    // 4. 释放临时值
    JS_FreeValue(m_ctx, resolving_funcs[0]);
    JS_FreeValue(m_ctx, resolving_funcs[1]);

    // 5. 返回已经 resolve 完成的 Promise
    return promise;
}

JSValue JZJSEngine::createRejectPromise(JSValue jsValue)
{
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(m_ctx, resolving_funcs);

    JS_Call(m_ctx, resolving_funcs[1], JS_UNDEFINED, 1, &jsValue);

    JS_FreeValue(m_ctx, resolving_funcs[0]);
    JS_FreeValue(m_ctx, resolving_funcs[1]);

    return promise;
}

void JZJSEngine::setProperty(JSValue obj, std::string name, JSValue value)
{
    JS_SetPropertyStr(m_ctx, obj, name.c_str(), value);
}

JSValue JZJSEngine::getProperty(JSValue obj, std::string name)
{
    return JS_GetPropertyStr(m_ctx, obj, name.c_str());
}

JZJSValuePtr JZJSEngine::makeValuePtr(JSValue value)
{
    return JZJSValuePtr(m_ctx, value);
}

void JZJSEngine::setPromiseResolve(int handle, const std::vector<JZJSValuePtr>& result)
{
    PromiseTask* task = getPromiseTask(handle);
    if (!task)
        return;

    std::vector<JSValue> value_list;
    for (int i = 0; i < result.size(); i++)
    {
        value_list.push_back(result[i].get());
    }

    JSValue ret = JS_Call(m_ctx, task->resolve, JS_UNDEFINED, value_list.size(), value_list.data());
    map_remove(m_promiseTask, handle);
}

void JZJSEngine::setPromiseReject(int handle, const std::string& error_message)
{
    PromiseTask* task = getPromiseTask(handle);
    if (!task)
        return;

    JSValue error = JS_NewError(m_ctx);
    JSValue msg = JS_NewString(m_ctx, error_message.c_str());
    JS_SetPropertyStr(m_ctx, error, "message", msg);

    JSValue args[] = {error};
    JSValue ret = JS_Call(m_ctx, task->reject, JS_UNDEFINED, 1, args);
    JS_FreeValue(m_ctx, error);

    map_remove(m_promiseTask, handle);
}

void* JZJSEngine::constructor(int type)
{
    return m_typeMap[type].classDefine.create();
}

void JZJSEngine::deconstructor(JSValue value)
{
    int type = jsValueType(value);
    assert(type != JSType_none);

    JSClassID class_id = JS_GetClassID(value);

    auto ptr = JS_GetOpaque(value, class_id);
    m_typeMap[type].classDefine.destory(ptr);
}

JSValue JZJSEngine::call(int magic, int argc, JSValueConst* argv)
{
    CFunction* func = m_funcList[magic].get();
    if (argc != func->params.size())
    {
        std::string error = "参数错误:需要" + to_string(func->params.size()) + "个参数";
        return JS_ThrowTypeError(m_ctx, "%s", error.c_str());
    }

    for (int i = 0; i < (int)func->params.size(); i++)
    {
        int arg_type = func->params[i].type;
        int js_type = jsValueType(argv[i]);
        if (js_type != arg_type)
        {
            string input_type = typeName(js_type);
            string need_type = typeName(arg_type);
            std::string error = "参数错误:参数" + to_string(i) + "需要类型" + need_type + "，输入是" + input_type;
            return JS_ThrowTypeError(m_ctx, "%s", error.c_str());
        }
    }

    // try
    {
        return func->call(m_ctx, argv);
    }
    // catch (std::exception& e)
    {
        // return JS_ThrowInternalError(m_ctx, "%s", e.what());
    }
}

JSValue JZJSEngine::callAsync(int magic, int argc, JSValueConst* argv)
{
    CFunction* func = m_funcList[magic].get();
    if (argc != func->params.size() - 1)
    {
        std::string error = "参数错误:需要" + to_string(func->params.size() - 1) + "个参数";
        return JS_ThrowTypeError(m_ctx, "%s", error.c_str());
    }

    for (int i = 0; i < argc; i++)
    {
        int arg_type = func->params[i + 1].type;
        int js_type = jsValueType(argv[i]);
        if (js_type != arg_type)
        {
            string input_type = typeName(js_type);
            string need_type = typeName(arg_type);
            std::string error = "参数错误:参数" + to_string(i) + "需要类型" + need_type + "，输入是" + input_type;
            return JS_ThrowTypeError(m_ctx, "%s", error.c_str());
        }
    }

    int promise_handle = m_promiseTaskId++;

    std::shared_ptr<PromiseTask> task = std::make_shared<PromiseTask>();

    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(m_ctx, resolving_funcs);
    task->resolve = JS_DupValue(m_ctx, resolving_funcs[0]);
    task->reject = JS_DupValue(m_ctx, resolving_funcs[1]);
    m_promiseTask[promise_handle] = task;

    JSValue handle_value = JS_NewInt32(m_ctx, promise_handle);
    try
    {
        vector<JSValue> async_args;
        async_args.push_back(handle_value);
        for (int i = 0; i < argc; i++)
        {
            async_args.push_back(argv[i]);
        }
        func->call(m_ctx, async_args.data());

        JS_FreeValue(m_ctx, promise);
        JS_FreeValue(m_ctx, handle_value);
        return promise;
    }
    catch (std::exception& e)
    {
        map_remove(m_promiseTask, promise_handle);
        JS_FreeValue(m_ctx, promise);
        JS_FreeValue(m_ctx, handle_value);

        return JS_ThrowInternalError(m_ctx, "%s", e.what());
    }
}

std::string JZJSEngine::runtimeError()
{
    return m_runtimeError;
}

void JZJSEngine::onExceptionOccurred(std::string error)
{
    m_runtimeError = error;

    setStatus(Engine_error);
    stopEngineTimer();

    sigLog.emit(m_runtimeError);
    sigError.emit(m_runtimeError);
}

JSContext* JZJSEngine::context()
{
    return m_ctx;
}

JZJSValuePtr JZJSEngine::eval(const std::string& code)
{
    JSValue result = JS_Eval(m_ctx, code.c_str(), code.size(), "eval", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result))
        unHandleException();

    return JZJSValuePtr(m_ctx, result);
}

JZJSValuePtr JZJSEngine::loadModule(const std::string& filepath)
{
    JZFile file;
    if (!file.open(filepath, "rt"))
    {
        JSValue error = JS_ThrowPlainError(m_ctx, "open path '%s; failed", filepath.c_str());
        return JZJSValuePtr(m_ctx, error);
    }

    JZByteArray js_bytecode = file.readAll();
    file.close();

    string code = str_fromUtf8(js_bytecode);

    JSValue result = JS_Eval(m_ctx, code.c_str(), code.size(), "eval", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result))
        unHandleException();

    return JZJSValuePtr(m_ctx, result);
}

void JZJSEngine::execOnce()
{
    if (isInterruption())
        return;

    // 执行 set timer
    while (true)
    {
        int64_t now = currentMs();
        int64_t max_gap = 0;

        int max_task = -1;
        for (int i = 0; i < (int)m_timeTasks.size(); i++)
        {
            int64_t cur_gap = now - m_timeTasks[i].next_run_time;
            if (cur_gap > max_gap)
            {
                cur_gap = max_gap;
                max_task = i;
            }
        }
        if (max_task == -1)
            break;

        JSValue result = JS_Call(m_ctx, m_timeTasks[max_task].callback, JS_UNDEFINED, 0, nullptr);
        if (JS_IsException(result))
        {
            unHandleException();
            JS_FreeValue(m_ctx, result);
            return;
        }

        JS_FreeValue(m_ctx, result);
        if (m_timeTasks[max_task].runOnce)
        {
            JS_FreeValue(m_ctx, m_timeTasks[max_task].callback);
            vec_removeAt(m_timeTasks, max_task);
        }
        else
        {
            m_timeTasks[max_task].next_run_time += m_timeTasks[max_task].interval;
        }
    }

    // 执行 promise
    JSContext* pend_ctx = nullptr;
    while (true)
    {
        int ret = JS_ExecutePendingJob(m_rt, &pend_ctx);
        if (ret == 0)
            break;
        else if (ret < 0)
        {
            unHandleException();
            return;
        }
        else if (ret == 1)
        {
            if (!m_promiseError.empty())
            {
                onExceptionOccurred(m_promiseError);
                m_promiseError.clear();
                return;
            }
        }
    }
}