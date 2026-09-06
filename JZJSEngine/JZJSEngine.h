#ifndef JS_ENGINE_H_
#define JS_ENGINE_H_

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "json/json.h"
#include "quickjs/quickjs.h"
#include "JZJSType.h"
#include "JZObject.h"

enum JZJSEngineStatus
{
    Engine_none,
    Engine_running,
    Engine_pause,
    Engine_error,
};

class JZJSValuePtr
{
public:
    JZJSValuePtr();
    JZJSValuePtr(JSContext* ctx, JSValue value);
    ~JZJSValuePtr();

    JSValue get() const;

protected:
    std::shared_ptr<void> m_guard;
    JSValue m_value;
};

// C++ 调用 JS 异步函数(callAsyncFunction)的结果
struct JZAsyncResult
{
    int result;                       // callAsyncFunction 返回的句柄
    std::vector<JZJSValuePtr> params; // promise resolve 的参数列表(一般 1 个元素)
};

// JZJSEngine
class JZJSEngineInternalFunction;
class JZJSEngine : public JZObject
{
public:
    JZJSEngine(JZObject* parent = nullptr);
    virtual ~JZJSEngine();

    void init();
    void deinit();

    void setWorkDir(const std::string& dir);

    void start();
    void stop();
    void resume();
    void pause();
    void resetUncaughtError();
    int status();

    JSContext* context();

    JZJSValuePtr eval(const std::string& code);
    JZJSValuePtr loadModule(const std::string& path);  // 这个实际是在全局空间

    // 同步调用全局 JS 函数。func = 全局函数名，params 原样透传给 JS。
    // 返回值的 1 元素 vector(返回 undefined 也有 1 个元素)；失败(函数不存在/调用抛异常)
    // 与 eval() 一致走 runtimeError/sigError 错误链，返回空 vector。
    std::vector<JZJSValuePtr> callFunction(const std::string& func, const std::vector<JZJSValuePtr>& params);

    // 调用全局 JS 异步函数(async function / 返回 PromiseLike)。发起后立即返回句柄(>=0)：
    // - promise resolve 后发 sigAsyncResult{ result=句柄, params=resolve 参数 }；
    // - promise reject 走引擎"未处理拒绝"上报(Engine_error/sigError/runtimeError)；
    // - 函数不存在/调用抛异常返回 -1；
    // - 返回值不是 PromiseLike(同步普通值)时立即发一次 sigAsyncResult。
    // 注意: async 函数体只同步执行到首个 await，后续靠引擎 job 泵推进 ——
    // 引擎须 start()(自带 10ms 定时器 pumpPendingJobs)或跑在事件循环里，否则结果不会回来。
    int callAsyncFunction(const std::string& func, const std::vector<JZJSValuePtr>& params);

    // 以 ESM 模块方式加载并执行文件(真正支持顶层 import/export)：
    // - filepath 用相对 workDir 的 '/' 路径或绝对路径；
    // - import 依赖由内置模块 loader 自动解析(扩展名可省略，自动补 .js / index.js)；
    // - bindExports=true 时把入口模块的命名导出绑定到全局变量(供后续全局 eval 访问)；
    // - 出错时与 eval() 一致：记录到 runtimeError(通过 sigError/sigLog 上报)。
    bool runModuleFile(const std::string& filepath, bool bindExports = false);
    void execOnce();

    // 引擎是否还有待执行的异步工作(接下来再调 execOnce/跑事件循环会有事做)：
    // - quickjs promise 微任务队列非空(JS_IsJobPending)；
    // - 还挂着 JS 侧 setTimeout/setInterval 定时器(含尚未到期与周期定时器)。
    bool hasPendingJob();

    bool isException(JSValue exc);
    std::string runtimeError();

    void setInterruption(bool flag);
    bool isInterruption() const;
    void wait(int ms);  // 会被打断

    int ctypeidToType(std::string ctypeid);
    int jsTypeToType(int js_type);
    int typeToJSType(int type);
    int jsValueType(JSValue v);

    bool checkType(JSValue value, int type);
    bool canConvert(int from_type, int to_type);

    JSValue globalObject();
    JSValue createObject();
    JSValue createValue(int type);
    JSValue createValue(int type, void* ptr);
    void freeValue(JSValue value);
    JSValue dupValue(JSValue value);
    JSValue createResolvedPromise(JSValue jsValue);
    JSValue createRejectPromise(JSValue jsValue);

    void setProperty(JSValue obj, std::string name, JSValue value);  // 不会拷贝, set后不能释放
    JSValue getProperty(JSValue obj, std::string name);

    template <class T>
    T fromJSValue(JSValue v)
    {
        return ::fromJSValue<T>(m_ctx, v);
    }

    template <class T>
    JSValue toJSValue(const T& t)
    {
        return ::toJSValue(m_ctx, t);
    }

    template <class T>
    JZJSValuePtr toValuePtr(const T& t)
    {
        return makeValuePtr(toJSValue(t));
    }

    JZJSValuePtr makeValuePtr(JSValue value);
    void setPromiseResolve(int handle, const std::vector<JZJSValuePtr>& result);
    void setPromiseReject(int handle, const std::string& error);

    void registFunction(JSValue this_val, std::string name, std::shared_ptr<CFunction> func);
    void registAsyncFunction(JSValue this_val, std::string name, 
        std::shared_ptr<CFunction> func);  // CFunction 第一个是promise handle, 后面是正常参数
    void registClass(const CClassDefine& define, int id = JSType_none);

    template <class T>
    void registVector(int id = JSType_none)
    {
        int object_id;
        if (id != JSType_none)
            object_id = id;
        else
            object_id = m_objectId++;

        int value_type = ctypeidToType(typeid(T).name());

        TypeInfo info;
        info.name = "vector<" + typeName(value_type) + ">";
        info.type = object_id;
        info.jsType = JS_TAG_OBJECT;
        info.ctypeId = typeid(std::vector<T>).name();
        info.valueType = value_type;
        registType(info);
    }

jz_signals:
    JZSigInt sigStatus;
    JZSigString sigLog;
    JZSigString sigError;
    // C++ 调用 JS 异步函数(callAsyncFunction)的结算回调。
    // 参数含自定义类型 JZAsyncResult，连接类型限 Auto/Direct(同线程)；
    // Queued/BlockingQueued 需要 JZVariant 注册，会断言。
    JZSignal<const JZAsyncResult&> sigAsyncResult;

protected:
    friend JZJSEngineInternalFunction;

    struct TypeInfo
    {
        TypeInfo();

        std::string name;
        int type;
        int jsType;  // 这里的jsType 是 tag 和 class_id 组合，有潜在问题，先这样
        std::string ctypeId;
        int valueType;
        CClassDefine classDefine;
        std::shared_ptr<std::vector<JSCFunctionListEntry>> funcList;
    };

    struct TimerTask
    {
        TimerTask();

        bool runOnce;
        JSValue callback;       // 回调函数
        int64_t interval;       // 间隔时间(ms)，0表示一次性任务
        int64_t next_run_time;  // 下次执行时间(ms，从纪元开始)
        uint32_t id;            // 任务ID
    };

    struct PromiseTask
    {
        PromiseTask();
        ~PromiseTask();

        JSContext* ctx;
        JSValue resolve;
        JSValue reject;
    };

    std::string workDir();
    std::string fullPath(std::string& path);

    std::string typeName(int type);

    bool isVectorType(int type);
    void print(JSValueConst this_val, int argc, JSValueConst* argv);
    std::string printObject(JSValue value);

    std::string makeTypeErrorText(int i, int type);
    std::string makeGetPropErrorText(std::string error);

    JSValue makeParamNumError(int num);
    JSValue makeTypeError(int i, int type);
    JSValue makeOpenFileError(std::string file);

    void* constructor(int type);
    void deconstructor(JSValue value);
    std::string getExceptionDescript(JSValue exc);

    int setInterval(JSValue callback, int64_t interval, bool once);
    void clearInterval(int id);
    JSValue call(int magic, int argc, JSValueConst* argv);
    JSValue callAsync(int magic, int argc, JSValueConst* argv);

    void unHandleException();
    void unHandlePromiseReject(JSValueConst promise, JSValueConst reason);
    PromiseTask* getPromiseTask(int handle);

    // callAsyncFunction 挂载的 promise resolve/reject 回调(由 JSInter 静态方法转发)
    void asyncResultResolve(int handle, int argc, JSValueConst* argv);
    void asyncResultReject(int handle, int argc, JSValueConst* argv);

    virtual void customEvent(JZEvent* event) override;
    virtual void timerEvent(JZTimerEvent* event) override;

    int64_t currentMs();
    void initTypeMap();
    void initJSEnv();
    void deinitJSEnv();

    void registInternal();
    void registConsole();

    bool load(const std::string& file);
    bool loadScript(const std::string& script);
    std::string getPropertyStr(JSValue var, std::string prop);

    // ---- ESM 模块支持(供 quickjs 模块 loader / runModuleFile 内部使用) ----
    // 编译 module_name 指定的文件为模块定义(找不到文件/编译失败时置 exception 返回 NULL)
    JSModuleDef* loadModuleDef(const std::string& module_name);
    // 读模块源码：相对 workDir(或绝对)路径，带 .js / index.js 扩展名兜底与 BOM 剥离
    bool readModuleCode(const std::string& module_name, std::string& code);
    // 把模块的命名空间导出绑定到全局变量(通过 __moduleBindGlobal__ 助手)
    bool bindModuleNamespace(JSModuleDef* m);
    // 泵 promise 任务队列(含未处理拒绝上报 Engine_error)
    void pumpPendingJobs();

    void updateFunction(std::shared_ptr<CFunction> func);
    void registType(TypeInfo type_info);
    void registBase(std::string name, int type, int js_type, std::string ctypeid);

    void setStatus(JZJSEngineStatus status);
    void onExceptionOccurred(std::string error);

    JSRuntime* m_rt;
    JSContext* m_ctx;
    JSValue m_global;
    int m_objectId;
    std::string m_workDir;

    void startEngineTimer();
    void stopEngineTimer();

    std::vector<std::shared_ptr<CFunction>> m_funcList;

    std::map<int, TypeInfo> m_typeMap;
    std::map<std::string, int> m_cTypeMap;
    std::map<int, int> m_jsTypeMap;

    int m_timeTaskId;
    std::vector<TimerTask> m_timeTasks;

    int m_promiseTaskId;
    std::map<int, std::shared_ptr<PromiseTask>> m_promiseTask;

    std::string m_runtimeError;
    std::string m_promiseError;
    int m_timer;
    JZJSEngineStatus m_status;
    bool m_interrupt;
};

#endif  // !JS_ENGINE_H_
