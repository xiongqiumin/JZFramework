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
    void execOnce();

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
        info.ctypeId = typeid(vector<T>).name();
        info.valueType = value_type;
        registType(info);
    }

jz_signals:
    JZSigInt sigStatus;
    JZSigString sigLog;
    JZSigString sigError;

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
