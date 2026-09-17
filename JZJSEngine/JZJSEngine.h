#ifndef JS_ENGINE_H_
#define JS_ENGINE_H_

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <cstring>

#include "json/json.h"
#include "quickjs/quickjs.h"
#include "JZByteArray.h"
#include "JZMutex.h"
#include "JZJSType.h"
#include "JZObject.h"
#include "JZMeta.h"   // JZ_DECLARE_METATYPE(见下面的 JZJSValuePtr 注册)

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

// 把 JZJSValuePtr 登记成 JZ 值系统的 metatype: 这样 JS 值(数组/对象/函数...)就能直接装进 JZVariant
// (属性系统等), 不需要给 JZ 值系统写一套 JS 树转换。
// 引用计数天然正确: 载荷是 shared_ptr guard —— 拷贝 = 共享同一份, 最后一个引用析构时才 JS_FreeValue,
// 正好对上 JZMeta<T>::clone() 的"data 赋值"语义以及 JZMeta 的析构。
JZ_DECLARE_METATYPE(JZJSValuePtr)

// C++ 调用 JS 异步函数(callAsyncFunction)的结果
struct JZAsyncResult
{
    int result;                       // callAsyncFunction 返回的句柄
    std::vector<JZJSValuePtr> params; // promise resolve 的参数列表(一般 1 个元素)
};

// checkAndSetArgs 的结果载体: 把每个实参转换后填进对应成员, 调用方按 format 声明的类型直接读取。
// 标量存值(int/double/bool)、类对象存裸指针(void* p)、string/Json/vector 存堆指针(由析构自动释放)。
struct ArgResult
{
    enum Kind { kInvalid, kInt, kDouble, kBool, kPointer,
                kString, kJson,
                kVecInt, kVecDouble, kVecString, kVecJson, kVecPtr };
    Kind kind = kInvalid;
    union
    {
        int i;
        double d;
        bool b;
        void* p;                          // 类对象: 底层对象裸指针
        std::string* str_ptr;
        Json::Value* json_ptr;
        std::vector<int>* vec_int_ptr;
        std::vector<double>* vec_double_ptr;
        std::vector<std::string>* vec_string_ptr;
        std::vector<Json::Value>* vec_json_ptr;
        std::vector<void*>* vec_ptr_ptr;   // 类对象数组
    };

    ArgResult() {}
    ~ArgResult() { reset(); }
    ArgResult(ArgResult&& o) noexcept { moveFrom(std::move(o)); }
    ArgResult& operator=(ArgResult&& o) noexcept { if (this != &o) { reset(); moveFrom(std::move(o)); } return *this; }
    ArgResult(const ArgResult&) = delete;
    ArgResult& operator=(const ArgResult&) = delete;

    void reset()
    {
        switch (kind)
        {
        case kString: delete str_ptr; break;
        case kJson: delete json_ptr; break;
        case kVecInt: delete vec_int_ptr; break;
        case kVecDouble: delete vec_double_ptr; break;
        case kVecString: delete vec_string_ptr; break;
        case kVecJson: delete vec_json_ptr; break;
        case kVecPtr: delete vec_ptr_ptr; break;
        default: break;
        }
        kind = kInvalid;
    }

    void moveFrom(ArgResult&& o)
    {
        std::memcpy(this, &o, sizeof(*this));
        o.kind = kInvalid;
    }
};

// JZJSEngine
class JZJSEngineInternalFunction;
class JZJSWorkerModule;
class JZJSWorkerThread;
class JZJSWidgetModule;
class JZJSEngine : public JZObject
{
public:
    JZJSEngine(JZObject* parent = nullptr);
    virtual ~JZJSEngine();

    void init();
    void deinit();

    // ---- "当前 this" (self) ----
    // quickjs **没有**"取当前 this"的 API: this 只作为形参出现在"被调用的那个 C 函数"上
    // (JSCFunction / JSClassCall 的 this_val 参数); 从任意嵌套调用里反查调用者的 this 做不到
    // (JS 语言本身也不给, caller 都没有)。所以引擎自己维护一层调用栈, 两个方向都压:
    //   * JS -> C++ —— 成员方法 / 属性访问 / 注册函数进入时压 this_val;
    //   * C++ -> JS —— callMemberFunction 调回调时压被调用对象的句柄(回调是箭头函数时看不到 this)。
    // 退出时弹(异常安全, 见 SelfScope); 嵌套调用一律取栈顶(最近的那个)。
    //   * C++ 侧: 在 ClassBind::def 的 lambda 里调 self(), 拿到"我是谁"的那份 JS 包装对象;
    //   * JS 侧: 同名全局函数 self()(见 init 里注册的全局) —— 事件回调(onPaint / onClicked ...)
    //     写成箭头函数时拿不到 this, 用 self() 就能取回"这个回调是被哪个对象调起来的"。
    // 返回**借用**值(调用方不要 free; 只在本次调用期间有效); 不在上面的调用里时返回 undefined。
    JSValue self() const;

    // ---- "当前 this" 的压栈依据 ----
    // 判据是**注册路径**(谁把这个函数注册进来), 不是运行期看 this_val 像不像实例:
    //   * 类实例方法(ClassBind::def 的方法 / defProperty 的属性访问器, 即走引擎 magic 分发的那批)
    //     -> 占一帧, this = 类实例;
    //   * 命名空间 / 全局函数(registFunction / registAsyncFunction: fs.xxx / process.xxx / 模块导出)
    //     -> **不占帧**(this 只是"函数挂在哪个对象上", 不是实例) —— 里面 self() 看到的是外层的实例;
    //   * 裸 C 函数(ClassBind::def(name, JSCFunction*) / defData): 引擎不经手, 也不占帧 ——
    //     它们的 C 实现本来就拿得到 this_val 形参, 不需要引擎替它们记住;
    //   * 引擎 -> JS(callMemberFunction: 事件转发 / 信号回调) -> 占一帧, this = 被调用对象。
    // RAII: 压/弹这一帧
    class SelfScope
    {
    public:
        // 无脑占帧: 引擎主动以某对象为 this 调 JS(事件转发 / 信号回调)时用
        SelfScope(JZJSEngine* engine, JSValueConst self) : m_engine(engine)
        {
            push(self);
        }

        // 只有"类实例方法"才占帧: JS -> C++ 的入口(它们走 magic 分发)用这个版本
        SelfScope(JZJSEngine* engine, int magic, JSValueConst self) : m_engine(engine)
        {
            if (engine && magic >= 0 && magic < (int)engine->m_funcList.size())
            {
                const std::shared_ptr<CFunction>& func = engine->m_funcList[magic];
                if (func && func->memberFunction)   // 见 CFunction::bindInstance
                    push(self);
            }
        }

        ~SelfScope()
        {
            if (m_pushed)
                m_engine->m_selfStack.pop_back();
        }

    private:
        void push(JSValueConst self)
        {
            if (!m_engine)
                return;
            m_engine->m_selfStack.push_back(self);
            m_pushed = true;
        }

        JZJSEngine* m_engine = nullptr;
        bool m_pushed = false;
    };

    void setWorkDir(const std::string& dir);
    void setLibraryDir(const std::vector<std::string>& dirs);  // 模块加载额外查找目录(优先级低于 workDir)

    // ---- worker 支持 ----
    // 运行根 / 模块查找目录(只读)。worker 引擎继承同一套"运行环境",
    // 但**不继承任何 JS 状态**(全局变量/模块缓存各自一份)。
    std::string workDirPath() const { return m_workDir; }

    // 入口脚本(一般是 main.js)的绝对路径 = **第一个成功加载的模块**。appDir/appFile 以它为准
    // (QApplication.applicationDirPath()/applicationFilePath(), 见 bteserver/src/qtcompat.ts)。
    // 宿主可用 setAppFile 显式指定 —— 引擎启动时会预加载内部库(如 net/dist/index.js), 那个不该算入口。
    std::string appFilePath() const { return m_appFile; }
    void setAppFile(const std::string& file) { m_appFile = file; }
    std::vector<std::string> libraryDirs() const { return m_libraryDirs; }
    // C++ 侧派发的回调抛了未处理异常: 并入引擎错误链(sigLog/sigError + Engine_error)
    void reportUncaughtError(const std::string& text) { onExceptionOccurred(text); }

    // ---- worker 线程表(进程级, 挂在主引擎上) ----
    // 主引擎 = 进程里第一个 init() 的引擎。worker 线程由主引擎创建与回收,
    // 表也只有主引擎在用(worker 线程通过 JZJSEngine::mainEngine() 拿到它)。
    //
    // 投递落点 = "目标 worker 的引擎": 引擎是 JZObject, 在它自己的线程上,
    // 所以 jzApp->postEvent(目标引擎, 消息事件) 就能把消息送到那个线程,
    // 再由该引擎的 customEvent 转给它自己的 worker 模块(见 JZJSWorkerModule)。
    JZJSEngine* mainEngine();

    // 本引擎装配的 worker 模块(由 JZJSWorkerModule::initEngine 登记):
    // 它是消息事件与"worker 退出"事件的 JS 落点。
    void setWorkerModule(JZJSWorkerModule* module);
    JZJSWorkerModule* workerModule() const;

    // 本引擎装配的 widget 模块(由 JZJSWidgetModule::initEngine 登记):
    // 各个 JS 构造函数(js_JZWidget_ctor 系列)要拿它把新建的控件登记进控件表
    // (addWidget/setParentWidget 之外的另一条登记入口, 见 JZJSWidgetModule::registerCreatedWidget)。
    void setWidgetModule(JZJSWidgetModule* module);
    JZJSWidgetModule* widgetModule() const;

    // 创建 worker(任意线程可调: 内部保证在主线程建线程对象并等它就绪)。失败返回 -1。
    int spawnWorker(const std::string& entry, const JZByteArray& workerData,
                    int ownerId, int parentId,
                    const std::string& workDir, const std::vector<std::string>& libraryDirs);

    // 任意线程: 按 id 投一条消息; false = 不存在/未就绪/已退出
    bool postToWorker(int targetId, int fromId, const JZByteArray& data);

    // worker 线程: 引擎就绪后登记为投递落点 / 引擎销毁前摘除(此后不再被投递)
    void attachWorkerEngine(int id, JZJSEngine* engine);
    void detachWorkerEngine(int id);

    bool terminateWorker(int id);           // 请求某个 worker 退出(协作式)
    std::vector<int> workerIds();           // 当前所有 id(含主线程的 0)
    std::vector<int> workerChildIds(int parentId);
    void reapAllWorkers();                  // 停掉并回收全部 worker(主线程)

    void start();
    void stop();
    void resume();
    void pause();
    void resetUncaughtError();
    int status();

    JSContext* context();

    JZJSValuePtr eval(const std::string& code);  // 全局空间执行(JS_EVAL_TYPE_GLOBAL)


    // 同步调用全局 JS 函数。func = 全局函数名，params 原样透传给 JS。
    // 返回值的 1 元素 vector(返回 undefined 也有 1 个元素)；失败(函数不存在/调用抛异常)
    // 与 eval() 一致走 runtimeError/sigError 错误链，返回空 vector。
    std::vector<JZJSValuePtr> callFunction(const std::string& func, const std::vector<JZJSValuePtr>& params);
    std::vector<JZJSValuePtr> callMemberFunction(const std::string& func, const JZJSValuePtr &this_val,const std::vector<JZJSValuePtr>& params);

    // 调用全局 JS 异步函数(async function / 返回 PromiseLike)。发起后立即返回句柄(>=0)：
    // - promise resolve 后发 sigAsyncResult{ result=句柄, params=resolve 参数 }；
    // - promise reject 走引擎"未处理拒绝"上报(Engine_error/sigError/runtimeError)；
    // - 函数不存在/调用抛异常返回 -1；
    // - 返回值不是 PromiseLike(同步普通值)时立即发一次 sigAsyncResult。
    // 注意: async 函数体只同步执行到首个 await，后续靠引擎 job 泵推进 ——
    // 引擎须 start() 并跑在宿主事件循环里(promise 结算会通过 JZEngine_WakeUp 唤醒泵一轮)，
    // 否则结果不会回来。
    int callAsyncFunction(const std::string& func, const std::vector<JZJSValuePtr>& params);

    // 以 ESM 模块方式加载并执行文件(真正支持顶层 import/export)：
    // - filepath 用相对 workDir 的 '/' 路径或绝对路径；
    // - import 依赖由内置模块 loader 自动解析(扩展名可省略，自动补 .js / index.js)；
    // - 出错时与 eval() 一致：记录到 runtimeError(通过 sigError/sigLog 上报)。
    bool runModuleFile(const std::string& filepath);
    void execOnce();    

    // 引擎是否还有待执行的异步工作(接下来再调 execOnce/跑事件循环会有事做)：
    // - quickjs promise 微任务队列非空(JS_IsJobPending)；
    // - 还挂着 JS 侧 setTimeout/setInterval 定时器(含尚未到期与周期定时器)。
    bool hasPendingJob();

    // 外部(C++ 模块)挂起的异步工作计数：网络一类的原生异步任务在途时 +1, 结束时 -1。
    // hasPendingJob() 会把计数也算进去, 这样宿主在等原生结果时不会提前退出。
    void addPendingWork();
    void releasePendingWork();
    int pendingWork() const;

    bool isException(JSValue exc);
    std::string runtimeError();

    void setInterruption(bool flag);
    bool isInterruption() const;
    void wait(int ms);  // 会被打断

    int ctypeidToType(std::string ctypeid);
    std::string typeToCTypeid(int type);

    std::string typeToTypeName(int type_id);
    int typeNameToType(std::string type_name);

    int jsTypeToType(int js_type);
    int typeToJSType(int type);
    
    int jsValueType(JSValue v);
    bool isJSRawPointerClassId(JSClassID class_id) { return false;  }

    bool checkType(JSValue value, int type);
    bool checkType(JSValue value, std::string type_name);
    // 构造参数的"检测 + 取值"一步到位: format 形如 "int=0,int=0|array<int>[2]"(参考 TS 形参格式),
    // 多个 group 用 '|' 分隔对应 JS 重载。返回命中的 group 下标, 全不匹配则已抛 JS 异常并返回 -1。
    //   vector<ArgResult> r;
    //   int g = engine->checkAndSetArgs("int=0,int=0|array<int>[2]", &r);
    //   if (g == -1) return JS_EXCEPTION;
    //   if (g == 0) return new JZRect(r[0].i, r[1].i);
    //   vector<int>* v = r[0].vec_int_ptr; return new JZPoint((*v)[0], (*v)[1]);
    // 仅适用于构造器(JS new Cls(...)): 实参 argc/argv 取自首层 createValue。
    int checkAndSetArgs(const std::string& format, std::vector<ArgResult>* result);
    bool canConvert(int from_type, int to_type);

    // 单个形参的转换前校验(同步 call / 异步 callAsync 共用, 避免两条路校验不一致):
    // 类型(含 JSRawPointer 形参的精确类) + 包装是否已 clear。
    // 返回 false 表示校验失败, 且已 JS_Throw*(调用方直接返回 JS_EXCEPTION)。
    bool checkFunctionArg(const CFunctionParam& param, int index, JSValueConst value);

    // ---- 继承支持 ----
    // class_id -> 该类的 ctypeid(供 fromJSValue 向上转型时取真实类)
    std::string classCTypeId(JSClassID id) const;
    // 沿 super 链判断 derivedCTypeId 是否 is-a baseCTypeId(含自身)
    bool isInstanceOf(const std::string& derivedCTypeId, const std::string& baseCTypeId) const;
    // 返回某类型对应的 JS 构造器函数(供 fromJSValue 走 JS 原型链做 JS_IsInstanceOf 判定 is-a)
    JSValue classConstructor(int type) const;

    JSValue globalObject();
    JSValue createObject();
    JSValue createValue(int type, int argc, JSValueConst* argv);
    JSValue createValue(int type, jsbind::JSPointerBase *ptr);    

    void freeValue(JSValue value);
    JSValue dupValue(JSValue value);
    JSValue createResolvedPromise(JSValue jsValue);
    JSValue createRejectPromise(JSValue jsValue);

    void setProperty(JSValue obj, std::string name, JSValue value);  // 不会拷贝, set后不能释放
    JSValue getProperty(JSValue obj, std::string name);

    // ---- 指针语义对象的身份表(状态见 JZSType.h 的 JSPointerState) ----
    // key = 底层对象裸指针(getRaw())。同一个对象只对应一份状态 ⇒
    //   ① 不同包装共享 cppOwner/cleared -> 所有权翻转/失效同步;
    //   ② 托管了句柄(state->handle)后再包给 JS, 直接复用**同一个 JS 对象** -> `===` 成立;
    //   ③ 绑定层可以把"每对象状态"挂在 state->data 上(见 jsbind::JSPointerData / pointerDataList):
    //      键同样是底层对象, 于是一份状态就够当"对象 <-> 绑定层状态"的登记表, 不必再自建一张。
    // 值语义类型(默认, 见 CClassDefine::pointerSemantics)不进这张表, 状态各自一份。
    // create=false 只查; create=true 时表里没有就用 cppOwnerIfNew 建一份 —— 之所以要这个参数:
    // 建状态的是**第一次包装**那次调用(toJsValueFromPtr 传进来的 cppOwner 是调用方的意图),
    // 之后同对象的包装共享同一份状态, 各自传来的 cppOwner 不再生效(这正是"翻转同步"的前提)。

    jsbind::JSPointerBase* getValuePointer(JSValue value);
    void movePointerToCpp(jsbind::JSPointerBase *pointer, JSValue value);
    JSValue movePointerToJs(jsbind::JSPointerBase *pointer);
    void detachPointerCpp(jsbind::JSPointerBase *pointer);

    jsbind::JSPointerBase *getPointerFrowRaw(const void* ptr);
    JSValue getJSValueFromRaw(const void* ptr) const;
    void detachPointerFromRaw(const void* ptr);

    // 对外只暴露 值/const 值/引用/const 引用/指针 四种类对象提取形式:
    //   engine.fromJSValue<Class*> / <Class> / <Class&> / <const Class&>
    // JSRawPointer<Class> 的提取不对外提供(内部由 call() 经自由 ::fromJSValue<JSRawPointer<Class>> 转发)。
    template <class T>
    std::enable_if_t<!jsbind::is_class_pointer<std::remove_cv_t<std::remove_reference_t<T>>>::value, T>
    fromJSValue(JSValue v)
    {
        return ::fromJSValue<T>(m_ctx, v);
    }

    template <class T>
    JSValue toJSValue(const T& t)
    {
        return ::toJSValue(m_ctx, t);
    }
    
    template <class T>
    JSValue toJsValueFromPtr(T* ptr, bool cppOwner)
    {
        if (!ptr)
            return JS_NULL;

        auto pre_ptr = getPointerFrowRaw(ptr);
        if (pre_ptr)
        {
            JZ_ASSERT_X(cppOwner, "cppOwner = false, 使用 moveToJson");
            return JS_DupValue(m_ctx, pre_ptr->m_state->jsValue);
        }

        jsbind::JSPointerBase *base = jsbind::make_js_pointer(this, ptr);        
        if (cppOwner)
            base->m_state->cppOwner = true;

        int type = ctypeidToType(typeid(*ptr).name());
        if (type == JSType_none)
            type = ctypeidToType(typeid(T).name());

        return createValue(type, base);
    }

    template <class T>
    JZJSValuePtr toValuePtr(const T& t)
    {
        return makeValuePtr(toJSValue(t));
    }

    // 裸指针: 包装为 JSRawPointer。
    // cppOwner=true (默认): C++ 拥有底层对象, JS 仅持句柄, 不会 delete 外部指针, cpp 侧一定要在失效的时候调用
    // cppOwner=false: JS 拥有, JS 对象 GC 时释放底层对象(须由 new 分配, 勿传栈/外部托管指针)。
    template <class T>
    JZJSValuePtr toValuePtrFromPtr(T* ptr, bool cppOwner)
    {
        return makeValuePtr(toJsValueFromPtr(ptr, cppOwner));
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

    // ------------------------------------------------------------------
    // JS 回调的 GC 根 (绑定层把"只被 C++ 持有"的 JS 函数登记进来)
    // ------------------------------------------------------------------
    // 为什么需要: quickjs 的循环 GC 会回收"引用计数 > 0 但从根不可达"的对象。回调闭包常常
    // 与它捕获的 JS 包装成环(包装 -> C++ 对象 -> 回调槽 -> 闭包 -> 包装), 整条环从根不可达 ⇒
    // 被回收 ⇒ C++ 侧那份 JSValue 成了悬垂(下次调用就是 UAF)。把回调往一个**挂在全局对象上的
    // 数组**里也放一份之后, 它始终可达, 循环 GC 就不会动它。
    // 用法: 存回调时 `id = retainCallback(cb)`; 丢弃回调时 `releaseCallback(id, cb)` ——
    // 第二个参数是绑定层自己 dup 的那份(本函数负责 free, 与一次性释放路径统一)。
    // 使用者: JZJSNetModule(__jz.net 包装的 on/off 回调) / JZJSWidgetModule(控件回调槽)。
    int retainCallback(JSValueConst cb);           // 返回登记 id; 引擎已关/失败返回 -1
    void releaseCallback(int id, JSValue value);   // 取消登记(置空根) + 放掉这份引用

protected:
    friend JZJSEngineInternalFunction;

    struct TypeInfo
    {
        TypeInfo();

        std::string name;
        int type;
        int jsType;     // 这里的jsType 是 tag 和 class_id 组合，有潜在问题，先这样
        std::string ctypeId;
        int valueType;  // 值类型,对于vector类有效
        // 继承: 单继承下记录直接基类 ctypeid(供 canConvert 做类型兼容判定)
        std::string superCTypeId;
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
    void clearPointerStates();

    void registInternal();
    void registConsole();

    std::string getPropertyStr(JSValue var, std::string prop);
    // JS 值 -> 可读文本; 转不了(symbol 等)时吞掉残留异常并返回空串
    std::string jsValueToStr(JSValue value);


    // ---- ESM 模块支持(供 quickjs 模块 loader / runModuleFile 内部使用) ----
    // 编译 module_name 指定的文件为模块定义(找不到文件/编译失败时置 exception 返回 NULL)
    JSModuleDef* loadModuleDef(const std::string& module_name);
    // 读模块源码：相对 workDir(或绝对)路径，带 .js / index.js 扩展名兜底与 BOM 剥离。
    // resolved 非空时回填**实际命中的绝对路径**(供 appFile 记录第一个加载的脚本)
    bool readModuleCode(const std::string& module_name, std::string& code,
                        std::string* resolved = nullptr);
    // 泵 promise 任务队列(含未处理拒绝上报 Engine_error)
    void pumpPendingJobs();

    void updateFunction(std::shared_ptr<CFunction> func);
    void registType(TypeInfo type_info);
    void registBase(std::string name, int type, int js_type, std::string ctypeid);

    void setStatus(JZJSEngineStatus status);
    void onExceptionOccurred(std::string error);

    JSRuntime* m_rt;
    JSContext* m_ctx;
    int m_currentArgc = 0;             // checkAndSetArgs 读取的当前构造实参个数/数组(由 createValue 设置)
    JSValueConst* m_currentArgv = nullptr;
    JSValue m_global;
    int m_objectId;
    std::string m_workDir;
    std::string m_appFile;    // 入口脚本绝对路径(见 appFilePath); 空 = 没加载过脚本
    std::vector<std::string> m_libraryDirs;   // 模块加载时额外查找的根目录列表

    // ---- 事件驱动调度 ----
    // 引擎不再常驻 10ms 周期定时器, 而是:
    //  - 有 JS 定时任务时, 按最近到期时刻挂一次性 timer(scheduleNextTimer);
    //  - 没有任务完全停表(空闲零唤醒); C++ 结算 promise / 外部注入任务时,
    //    wakeUp() 投递 JZEngine_WakeUp 自定义事件唤醒引擎线程泵一轮(execOnce)。
    void scheduleNextTimer();
    void wakeUp();
    void startEngineTimer();   // 恢复/继续驱动(= scheduleNextTimer)
    void stopEngineTimer();    // 停表(暂停/停止/出错/析构)

    // 注册函数(def / defProperty / defData 的实现对象): 引擎持有它们, 所以它们的 name 也是
    // "可以发给 quickjs 当裸指针"的稳定宿主(见 registClass 里 entry.name = func->name.c_str())。
    std::vector<std::shared_ptr<CFunction>> m_funcList;

    // "当前 this" 调用栈: 值都是**借用**(被调用方形参的 this_val), 不 dup/不释放 —— 只在
    // 该次 JS -> C++ 调用期间有效(栈只在这段时间里非空)。见 self() / SelfScope。
    std::vector<JSValue> m_selfStack;

    std::map<int, TypeInfo> m_typeMap;
    std::map<std::string, int> m_cTypeMap;
    std::map<int, int> m_jsTypeMap;
    std::vector<jsbind::JSPointerBase*> m_jsPoints;

    // 回调 GC 根数组(全局 __jzCallbackRoot 里那份的宿主)与登记计数, 见 retainCallback。
    JSValue m_cbRoot = JS_UNDEFINED;
    int m_cbId = 0;

    int m_timeTaskId;
    std::vector<TimerTask> m_timeTasks;

    int m_promiseTaskId;
    std::map<int, std::shared_ptr<PromiseTask>> m_promiseTask;

    std::string m_runtimeError;
    std::string m_promiseError;
    int m_timer;
    int64_t m_timerExpire;   // 已挂一次性 timer 记录的到期时刻(scheduleNextTimer 维护)
    int m_pendingWork;       // 外部模块挂起的原生异步任务数量(见 addPendingWork)
    JZJSEngineStatus m_status;
    bool m_interrupt;    

    // ---- worker 线程表(仅主引擎会填; 见 worker 相关接口) ----
    struct WorkerEntry
    {
        WorkerEntry();

        int parentId;                  // 父 worker id(级联终止用)
        int ownerId;                   // 创建者 id(退出事件回投给它)
        bool exited;
        JZJSWorkerThread* thread;      // 线程对象(归主线程, 由主引擎回收)
        JZJSEngine* engine;            // 该 worker 的引擎 = 投递落点; 未就绪为空
    };

    int doSpawnWorker(const std::string& entry, const JZByteArray& workerData,
                      int ownerId, int parentId,
                      const std::string& workDir, const std::vector<std::string>& libraryDirs);
    void reapWorker(int id);            // 仅主线程: join + delete + 摘表
    void onWorkerFinished(int id);      // 收到 worker 的 sigFinished(已排队到主线程): 回收 + 级联 + 转告 owner
    int workerOwnerId(int id);

    JZJSEngine* m_mainEngine = nullptr;
    JZMutex m_workerMutex;
    int m_nextWorkerId;                 // 0 保留给主线程
    std::map<int, WorkerEntry> m_workers;
    JZJSWorkerModule* m_workerModule;   // 本引擎的 worker 模块(未装为空)
    JZJSWidgetModule* m_widgetModule;   // 本引擎的 widget 模块(未装为空; 见 setWidgetModule)
};

template <class T>
JSValue toJsValueFromPtr(JSContext* ctx, T* ptr, bool cppOwner)
{
    JZJSEngine* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    return engine->toJsValueFromPtr(ptr, cppOwner);
}

#endif  // !JS_ENGINE_H_
