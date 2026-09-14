#ifndef JS_TYPE_H_
#define JS_TYPE_H_

#include <assert.h>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "json/json.h"
#include "quickjs/quickjs.h"
#include "JZObject.h"
#include "JZObjectPointer.h"

class JZJSEngine;   // 前向声明: fromJSValuePointer 统一处理原始类/JSRawPointer 时需查询类型集合

namespace jsbind
{

// 非模板基类: 让检测阶段(不依赖具体 Class)即可判断包装指针是否已失效。
class JSPointerBase
{
public:
    virtual ~JSPointerBase() = default;
    virtual bool isCleared() const = 0;
    virtual bool cppOwner() const = 0;
    // 返回底层对象裸指针(继承向上转型时, 无需知道具体 Class 模板参数即可取对象指针)
    virtual void* getRaw() const = 0;
    // 所有权翻转(运行期可变)。绑定层实现 SIP 的 /Transfer/ /TransferBack/ 时经此翻转,
    // 好处是**不需要知道具体的包装类型** —— 例如 JZJSWidget 拿到自己的包装只要一个
    // JSPointerBase*, 就能把"自己"的所有权交给父控件。
    virtual void setCppOwner(bool owner) = 0;
    // 让句柄失效(底层对象仍由 C++ 管理; 仅 cppOwner == true 时允许, 各实现内部有断言)。
    // 提到基类的原因: JS 侧接口要在**只知道包装基类**的前提下, 让"已经发给 JS 的旧包装"立刻失效
    // (典型: JZJSWidget.cpp 的布局缓存 —— 布局包装的具体类型随类层次不同, 是 JSRawPointer<具体类>)。
    virtual void clear() = 0;
};

// ==================== JS 侧指针包装 ====================
// 两类包装都表示"JS 侧持有的一个 C++ 对象引用", 区别只在**失效方式**:
//   JSObjectPointer (只服务 JZObject 派生类): 内部复用 JZCore 的 JZObjectPointer 守卫指针,
//       对象析构时守卫自动置空 -> get() 返回 nullptr, 且对所有副本同时生效。
//       故 JZObject 派生类**不需要缓存包装**, 每次现造即可(守卫指针本身在共享 m_data 上)。
//   JSRawPointer<T> (非 JZObject 类型):  内部只有裸指针, 对 delete 无感知 -> 失效必须有人
//       显式 clear()。因为 PointInfo 由 shared_ptr 共享, 一次 clear() 会让该对象的**所有副本**
//       (含已经发给 JS 的那些)同时失效。典型用法: 拥有方在自己成员里缓存一份作为"失效开关",
//       在删除目标对象(或替换它)之前先 clear()。
//
// 所有权 (cppOwner) 是**运行期可翻转**的:
//   false = JS 拥有, 最后一个包装析构时 delete 底层对象;
//   true  = C++ 拥有, JS 仅持句柄, 不负责释放(clear() 只清句柄)。
// 删除动作放在 PointInfo 的析构里(而不是每个包装的析构), 于是:
//   - 只有最后一个引用消失时才删除 -> 临时拷贝 / 按值传参不会误删;
//   - 析构时才读 cppOwner -> setCppOwner() 的翻转真正生效
//     (对应 SIP 的 /Transfer/ /TransferBack/、Shiboken 的 setOwnership())。
// 另: 取回包装的形参请用引用(auto& cp = fromJSValue<...>), 共享的是同一个 PointInfo,
//     因此 setCppOwner / clear 能传播回 JS 持有的那份包装。

// 非模板: 只用于 JZObject 派生类。
class JSObjectPointer : public JSPointerBase
{
public:
    struct PointInfo
    {
        bool cppOwner = true;
        bool cleared = false;
        JZObjectPointer obj;

        PointInfo(JZObject* ptr, bool owner)
            : cppOwner(owner)
        {
            if (ptr)
                obj = ptr->guardPointer();
        }

        ~PointInfo()
        {
            // 最后一个引用消失时按**最终** cppOwner 决定是否释放(翻转能生效)。
            // 对象若已被 C++ 销毁, 守卫指针为 null, 这里不会重复删除。
            if (!cppOwner)
            {
                JZObject* p = obj.object();
                if (p)
                    delete p;
            }
        }
    };

    JSObjectPointer() {}

    JSObjectPointer(JZObject* ptr, bool cppOwner)
    {
        m_info = std::make_shared<PointInfo>(ptr, cppOwner);
    }

    // 底层对象裸指针; 对象已销毁或已 clear 时返回 nullptr。
    JZObject* get() const
    {
        if (!m_info || m_info->cleared)
            return nullptr;
        return m_info->obj.object();
    }

    bool isCleared() const override { return get() == nullptr; }
    bool cppOwner() const override { return !m_info || m_info->cppOwner; }
    void* getRaw() const override { return get(); }

    // 所有权翻转(借用 <-> 拥有); PointInfo 共享, 故对 JS 侧那份同样生效。
    void setCppOwner(bool owner) override
    {
        if (m_info)
            m_info->cppOwner = owner;
    }

    // 失效: 释放 JS 侧句柄, 底层对象仍由 C++ 管理(仅 cppOwner == true 时允许)。
    void clear()
    {
        JZ_ASSERT(!m_info || m_info->cppOwner);
        if (m_info)
            m_info->cleared = true;
    }

private:
    std::shared_ptr<PointInfo> m_info;
};

template <class Class>
class JSRawPointer : public JSPointerBase
{
public:
    struct PointInfo
    {
        Class* ptr = nullptr;
        bool cppOwner = true;
        bool cleared = false;

        PointInfo(Class* p, bool owner)
            : ptr(p)
            , cppOwner(owner)
        {
        }

        ~PointInfo()
        {
            // 最后一个引用消失时按**最终** cppOwner 决定是否释放(翻转能生效)。
            if (!cppOwner && ptr)
                delete ptr;
        }

        // 让所有共享该状态的包装(含 JS 侧那份)一起失效。
        // 与 clear() 的区别: 不检查 cppOwner、不需要拿到包装本身 —— C++ 侧在删除底层对象后
        // 可直接用持有着的 Info 标记失效(见 JZJSPaintScope 的绘制作用域)。
        void invalidate() { cleared = true; }
    };

    using Info = std::shared_ptr<PointInfo>;

    JSRawPointer() {}

    JSRawPointer(Class* ptr, bool cppOwner)
    {
        m_info = std::make_shared<PointInfo>(ptr, cppOwner);
    }

    // 句柄状态(与所有副本 / JS 侧那份包装共享)。
    // 拿到它就可以在"JS 对象可能已被回收"的情况下安全地失效句柄 —— 不必 dup JS 对象、
    // 也不必保证包装本身还活着(状态是 shared_ptr 独立存活的)。
    Info info() const { return m_info; }

    // 底层对象裸指针; 已 clear 时返回 nullptr。
    Class* get() const
    {
        if (!m_info || m_info->cleared)
            return nullptr;
        return m_info->ptr;
    }

    bool isCleared() const override { return get() == nullptr; }
    bool cppOwner() const override { return !m_info || m_info->cppOwner; }
    void* getRaw() const override { return get(); }

    // 所有权翻转: JS 拥有(false) <-> C++ 拥有(true)。
    // 典型场景: list.addItem(item) / widget.setLayout(l) —— JS 创建的对象被 C++ 接管。
    void setCppOwner(bool owner) override
    {
        if (m_info)
            m_info->cppOwner = owner;
    }

    // 失效: 置空句柄; 底层对象仍由 C++ 管理(仅 cppOwner == true 时允许)。
    void clear()
    {
        JZ_ASSERT(!m_info || m_info->cppOwner);
        if (m_info)
            m_info->cleared = true;
    }

private:
    std::shared_ptr<PointInfo> m_info;
};

// JSRawPointer 萃取: 仅当 T 为 jsbind::JSRawPointer<X> 时 type 存在。
namespace detail
{
template <class T>
struct class_pointer_inner
{
};
template <class Class>
struct class_pointer_inner<JSRawPointer<Class>>
{
    using type = Class;
};
}  // namespace detail

template <class T>
struct is_class_pointer : std::false_type
{
};
template <class Class>
struct is_class_pointer<JSRawPointer<Class>> : std::true_type
{
};
}  // namespace jsbind

namespace jstype
{
// 兜底空包装: 仅供"上层漏校验"的不可达分支返回 —— 使返回值仍是合法对象(get()==nullptr),
// 断言之后不至于落进 UB; 调用方按空包装处理(与传 null / 已失效包装同一处理路径)。
template <class CP>
CP& emptyWrapper()
{
    static CP s_empty;
    return s_empty;
}
}   // namespace jstype

namespace jstype
{

template <typename T>
using remove_cv_t = typename std::remove_cv<T>::type;
template <typename T>
using remove_reference_t = typename std::remove_reference<T>::type;
template <typename T>
using remove_cvr_t = remove_cv_t<remove_reference_t<T>>;

// vector
template <class T>
struct is_vector : std::false_type
{
};

template <class T, class Alloc>
struct is_vector<std::vector<T, Alloc>> : std::true_type
{
};

template <class T>
struct vector_value_type
{
};

template <class T, class Alloc>
struct vector_value_type<std::vector<T, Alloc>>
{
    using type = T;
};

template <typename T, typename U>
void check_forbid_reference()
{
    // 1. 判断 T 是否为引用类型
    constexpr bool is_ref = std::is_reference<T>::value;
    // 2. 剥离引用后，获取原始类型（去 const 可选）
    using raw_type = std::remove_reference_t<T>;
    using unqualified_type = std::remove_const_t<raw_type>;  // 忽略 const 修饰
                                                             // 3. 判断原始类型是否为 U
    constexpr bool is_target_type = std::is_same<unqualified_type, U>::value;

    // 静态断言：若 T 是引用且原始类型为 U，则编译失败
    static_assert(!(is_ref && is_target_type), "禁止使用的引用类型");
}

}  // namespace jstype

enum
{
    JSType_pointerFlag = 1 << 30,

    JSType_none = -1,

    JSType_oriValue,  // 原始的JSValue
    JSType_bool,
    JSType_int,
    JSType_double,
    JSType_string,

    JSType_object = 1000,
    JSType_jsonValue,

    JSType_objectUser = 2000,
};

struct CFunctionParam
{
    CFunctionParam();

    std::string name;
    std::string ctypeid;
    int type;

    // 形参是 JSRawPointer<Class>(共享包装引用, 改动要能传播回 JS 那份)时置 true:
    // 这类形参只接受**精确类**, 派生类对象不能传(会共享不到同一个包装 -> 重复 delete / clear 不传播)。
    // 由 JZJSEngine::call() 在转换前按此标记校验, 免得落到底层断言。
    bool exactClass = false;
};

class CFunction
{
public:
    CFunction();
    virtual ~CFunction();

    virtual JSValue call(JSContext* ctx, JSValueConst* argv) = 0;

    std::string name;
    CFunctionParam result;
    std::vector<CFunctionParam> params;
    bool returnCppOwner = false;   // 返回值所有权: 注册返回裸指针的函数时记录; false=JS 拥有(GC 时 delete), true=C++ 持有(JS 不释放)

    // 这个函数是不是"**类实例方法**"(挂在类原型上的: ClassBind::def 的方法 / defProperty 的访问器)。
    // 判据来自注册路径, 不是运行期猜的:
    //   * registClass   -> 原型方法/属性访问器: 形参表首参就是"对象自己"(见派生的约定), this = 类实例;
    //   * registFunction-> 挂任意对象上的普通函数(全局 / 命名空间 / 模块导出): 形参就是普通实参, this 只是"挂在哪",
    //                     不是实例 -> 不算实例方法(回调里 self() 不该看到它)。
    // 用途: JS -> C++ 入口据此决定要不要把 this 压进引擎的"当前 this"栈(见 JZJSEngine::self)。
    // 注: 裸 C 函数(CRawFunction / CDataFunction)不参与 —— 它们的 C 实现本来就拿得到 this_val 形参,
    //     不需要引擎替它们记(self() 也就不会为它们单独起一帧)。
    bool bindInstance = false;
};

class CProperty
{
public:
    std::string name;

    std::shared_ptr<CFunction> read;
    std::shared_ptr<CFunction> write;
};

// 直接用 quickjs C 函数注册的方法(见 ClassBind::def(name, JSCFunction*))。
// 与 CFunction 的分工:
//   CFunction  —— 走引擎的 magic 分发(JS_CFUNC_generic_magic → js_member_function),
//                 形参由注册时生成的类型表校验后转成 C++ 实参再调 lambda; **拿不到 this_val**;
//   CRawFunction —— 不生成类型表、不进 magic 表: quickjs 以 JS_CFUNC_generic **原样**调用
//                 JSValue fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)。
//                 所以它拿得到 this_val 和原始 argv(需要翻接收者所有权、要返回真 null、
//                 要自定义重载分派的接口只能这么注册), 校验与错误提示全部自理。
struct CRawFunction
{
    std::string name;
    JSCFunction* function = nullptr;
    int length = 0;   // JS 侧 Function.length 的展示值(0 = 不定长); 不参与任何形参校验
};

// 带 func_data 的 C 函数(见 ClassBind::defData)。
// JS_CFUNC_generic 系列**没有数据槽**, 只有 JS_NewCFunctionData2 造出来的函数对象才有:
// 注册时给的 data 数组存在函数对象的记录里(GC 会 mark/free), 调用时原样交给第 6 个参数 ——
//   JSValue fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic,
//              JSValueConst* func_data)
// 用途: C 函数要拿到"注册它的人/上下文"(如 JZJSWidgetModule 指针)。this_val 是 JS 对象、
//      ctx opaque 只有引擎、magic 只能放一个 int, 都不够, 这类数据就只能随函数对象带。
// 注意: JSCFunctionListEntry 里没有数据字段, 所以这类函数注册时必须单独造函数对象再挂属性。
struct CDataFunction
{
    std::string name;
    JSCFunctionData* function = nullptr;
    int length = 0;               // JS 侧 Function.length(仅展示)
    int magic = 0;                // 原样透传给 C 函数
    std::vector<JSValue> data;    // 原样透传给 C 函数(注册时 quickjs 会对每个元素 dup 一份)
};

class CClassDefine
{
public:
    // 唯一构造入口: JS `new Cls(...)` 透传 (ctx, argc, argv) 生成底层对象并包成 JSRawPointer<Class>*。
    // 仅当类无默认构造(如 JZPainter 必须传 widget)或需要构造参数时才用 setCreate 覆盖。
    std::function<jsbind::JSPointerBase*(JSContext*, int, JSValueConst*)> createArgs;

    std::function<void(jsbind::JSPointerBase*)> destory;

    // 实例创建后钩子: createValue(type, ptr) 在 JS 对象建好并塞入 opaque 后调用一次
    // (覆盖 new / 返回值转换 / toValuePtr / createCanvas 全部包装路径, 因 toJsValueFromPtr → createJSValue → createValue(type, ptr))。
    // opaque 即 JSRawPointer<Class>*; 用于把 JSValue 句柄回写到 C++ 侧(控件的做法是给控件装一个
    // 事件转发过滤器 JZSWidgetEventFilter, 由它持有句柄并以该 JS 对象为 this 调方法, 见 JZJSWidgetModule.h)。
    // 由 setInstanceBind 注册。
    std::function<void(JSContext*, JSValue, jsbind::JSPointerBase* opaque)> onCreated;

    std::string name;
    std::string ctypeid;
    bool isPointer = false;   // true: 该类型对应 JSRawPointer<Class>, 检测阶段需判断是否已 clear

    // 继承: 单继承下记录直接基类 ctypeid(供 canConvert 做类型兼容判定)
    std::string superCTypeid;

    std::vector<CProperty> propertys;
    std::vector<std::shared_ptr<CFunction>> functions;

    // 带 func_data 的 C 函数(不进属性表: 注册时单独造函数对象再挂, 见 CDataFunction)
    std::vector<CDataFunction> dataFunctions;

    // 直接用 quickjs C 函数挂的方法(不进 magic 表; 见 CRawFunction)
    std::vector<CRawFunction> rawFunctions;
};

// from js
template <class T, std::enable_if_t<!jsbind::is_class_pointer<std::remove_cv_t<std::remove_reference_t<T>>>::value, int> = 0>
T fromJSValue(JSContext* ctx, JSValueConst value);

// 上层校验入口: 判断 JS 值是否是 Class(或 is-a 派生) 的注册对象。
// 供"没有参数类型表"的地方(如各类 createArgs / 构造参数)在转换前校验;
// 函数/方法形参的类型校验由 JZJSEngine::call() 按 CFunctionParam 完成。
// 校验通过后 fromJSValue 内部的类型不匹配分支就只剩 JZ_ASSERT 兜底(属于上层漏校验=框架 bug)。
template <class Class>
bool js_check_class_arg(JSContext* ctx, JSValueConst value, bool exact = false)
{
    if (JS_IsNull(value) || JS_IsUndefined(value))
        return false;

    JSClassID class_id;
    void* ptr = JS_GetAnyOpaque(value, &class_id);
    if (!ptr)
        return false;

    auto* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    if (!engine)
        return false;

    std::string actual = engine->classCTypeId(class_id);
    if (actual.empty())
        return false;

    std::string want = typeid(Class).name();
    return exact ? (actual == want) : engine->isInstanceOf(actual, want);
}

// T 为裸指针(Class*): 把 JS 对象转成 T*。JS 对象可能是两种表示(与 JZNodeEditor 的统一语义一致):
//   - 原始类(值/引用)对象: 其 opaque 本身就是真实对象指针, 直接转型返回即可;
//   - JSRawPointer<Class> 包装类: 其 opaque 是 JSRawPointer<Class>*, 取 get() (并判断是否已 clear)。
// null/undefined 映射为空指针。
// 类型校验不在这里做(见 js_check_class_arg / JZJSEngine::call): 走到不匹配分支说明上层漏校验,
// 只 JZ_ASSERT 报错 + 返回空指针(避免异常与 UB)—— 与 null/undefined 同语义。
template <class T>
T fromJSValuePointer(JSContext* ctx, JSValueConst value, std::true_type)
{
    static_assert(std::is_pointer<T>::value, "fromJSValuePointer<std::true_type>: 仅支持指针类型");
    using Class = std::remove_pointer_t<T>;

    // null / undefined 直接映射为空指针
    if (JS_IsNull(value) || JS_IsUndefined(value))
        return nullptr;

    // 必须是对象(原始类或 JSRawPointer 包装类); 主动判断 value 是否为对象
    JSClassID class_id;
    void* ptr = JS_GetAnyOpaque(value, &class_id);
    if (!ptr)
    {
        JZ_ASSERT_X(0, "fromJSValue<Class*>: 参数不是注册类对象(上层应已校验)");
        return nullptr;
    }

    // 区分原始类与 JSRawPointer 包装类: 后者登记在 m_JSRawPointerTypes 中
    auto* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    if (engine && engine->isJSRawPointerClassId(class_id))
    {
        auto* cpb = (jsbind::JSPointerBase*)ptr;
        if (cpb->isCleared())
        {
            // 运行期状态(不是类型不匹配): JS 侧调用已由 JZJSEngine::call()/callAsync 的预检拦下,
            // 走到这里说明有路径漏校验。断言(fatal) + 返回空指针(与 null/undefined 同语义)。
            JZ_ASSERT_X(0, "fromJSValue<Class*>: 传入已失效(clear)的包装, 上层应已拦截");
            JS_ThrowTypeError(ctx, "指针已被释放(clear)，无法访问");
            return nullptr;
        }

        // 统一包装模型: 派生类 JS 对象的 opaque 是 JSRawPointer<Derived>*, 形参可能要 Base*。
        // 真实类就是 Class 时直接取; 否则复用 JS 原型链判断 is-a(instanceof),
        // 单继承下直接把派生指针当基类指针用(偏移为 0), 无需 C++ upcast。
        std::string actual_cid = engine->classCTypeId(class_id);
        std::string want_cid = typeid(Class).name();
        if (actual_cid == want_cid)
        {
            // 经多态基类取底层指针: opaque 可能是 JSRawPointer<Class> 或 JSObjectPointer,
            // 不能按某一种具体包装类型强转。
            return (Class*)cpb->getRaw();
        }
        // is-a 判定: 走 JS 原型链(原型已在 registClass 时挂好), 不另维护平行类型表
        int base_type = engine->ctypeidToType(want_cid);
        if (base_type != JSType_none)
        {
            JSValue base_ctor = engine->classConstructor(base_type);
            bool is_base = (JS_IsInstanceOf(ctx, value, base_ctor) == 1);
            JS_FreeValue(ctx, base_ctor);
            if (is_base)
            {
                void* obj = cpb->getRaw();
                return (Class*)obj;
            }
        }
        JZ_ASSERT_X(0, "fromJSValue<Class*>: 参数类型不匹配(上层应已校验)");
        return nullptr;
    }

    // 原始类对象: opaque 即真实对象, 直接转型
    return (T)ptr;
}

template <class T>
T fromJSValueVector(JSContext* ctx, JSValueConst array, std::true_type)
{
    using ValueType = typename jstype::vector_value_type<T>::type;
    T vec;

    int64_t length;
    JS_GetLength(ctx, array, &length);
    for (int i = 0; i < length; i++)
    {
        JSValue sub = JS_GetPropertyUint32(ctx, array, i);
        ValueType v = fromJSValue<ValueType>(ctx, sub);
        JS_FreeValue(ctx, sub);

        vec.push_back(v);
    }
    return vec;
}

// JSRawPointer 在 check type 阶段的 clear 检查(仅对 JSRawPointer 类型生效)。
// 用 tag 派发(C++14 兼容, 不使用 if constexpr): true_type 分支做检查并可能抛 JS 异常。
template <class T>
void checkJSRawPointerCleared(JSContext* ctx, void* ptr, std::true_type)
{
    (void)sizeof(T);  // 保留 T 依赖, 避免未使用模板参数告警
    // 用 JSPointerBase 安全转型(派生类 JS 对象的 opaque 是 JSRawPointer<Derived>*, 无法按 Inner 强转)
    auto* cpb = (jsbind::JSPointerBase*)ptr;
    if (cpb->isCleared())
    {
        // 同 fromJSValuePointer: 上层(call/callAsync)已预检, 这里只兜底(断言 fatal, 不再继续取对象)。
        JZ_ASSERT_X(0, "fromJSValue: 传入已失效(clear)的 JSRawPointer, 上层应已拦截");
        JS_ThrowTypeError(ctx, "指针已被释放(clear)，无法访问");
        JZ_UNREACHABLE();
    }
}
template <class T>
void checkJSRawPointerCleared(JSContext*, void*, std::false_type)
{
}

template <class T>
T fromJSValueVector(JSContext* ctx, JSValueConst value, std::false_type)
{
    // string 和 Json::Value 是值语义, 所以不能使用引用
    jstype::check_forbid_reference<T, std::string>();
    jstype::check_forbid_reference<T, Json::Value>();

    JSClassID class_id;
    auto ptr = JS_GetAnyOpaque(value, &class_id);

    using Base = jstype::remove_cvr_t<T>;
    checkJSRawPointerCleared<Base>(ctx, ptr,
        std::integral_constant<bool, jsbind::is_class_pointer<Base>::value>());

    // JSRawPointer<Class> 形参已在上面的 is_class_pointer 分支处理(返回 wrapper 本身)。
    // 普通 Class 值/引用形参: 统一模型下其 opaque 始终是 JSRawPointer<Base>*, 取底层对象;
    // 值形参返回拷贝, 引用形参返回底层对象的左值引用(生命周期由 JS 对象担保)。
    if (!jsbind::is_class_pointer<Base>::value)
    {
        auto* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        if (engine && engine->isJSRawPointerClassId(class_id))
        {
            auto* cpb = (jsbind::JSPointerBase*)ptr;
            if (cpb->isCleared())
            {
                // 运行期状态: 上层(call/callAsync)已预检。值/引用返回没有安全的兜底值,
                // 故断言后直接 JZ_UNREACHABLE(致命), 绝不拿已失效包装取对象。
                JZ_ASSERT_X(0, "fromJSValue<Class&>: 传入已失效(clear)的包装, 上层应已拦截");
                JS_ThrowTypeError(ctx, "指针已被释放(clear)，无法访问");
                JZ_UNREACHABLE();
                return *((Base*)cpb->getRaw());   // 仅为满足签名: 上面已致命, 正常不会执行
            }

            // 真实类就是 Base 直接取; 派生类对象走 JS 原型链判断 is-a 后, 单继承下直接把派生指针当基类指针用
            std::string actual_cid = engine->classCTypeId(class_id);
            std::string want_cid = typeid(Base).name();
            if (actual_cid == want_cid)
            {
                // 同 fromJSValuePointer: 走多态基类取底层指针, 不按具体包装类型强转。
                return *((Base*)cpb->getRaw());
            }
            // is-a 判定: 复用 JS 原型链
            int base_type = engine->ctypeidToType(want_cid);
            if (base_type != JSType_none)
            {
                JSValue base_ctor = engine->classConstructor(base_type);
                bool is_base = (JS_IsInstanceOf(ctx, value, base_ctor) == 1);
                JS_FreeValue(ctx, base_ctor);
                if (is_base)
                {
                    void* obj = cpb->getRaw();
                    return *((Base*)obj);
                }
            }
            // 类型不匹配属于上层漏校验(call/callAsync 已按参数类型表拦下): 断言 + 兜底(值/引用无安全默认值)。
            JZ_ASSERT_X(0, "fromJSValue<Class&>: 参数类型不匹配(上层应已校验)");
            JS_ThrowTypeError(ctx, "参数类型不匹配: 期望 %s, 实际 %s", want_cid.c_str(), actual_cid.c_str());
            JZ_UNREACHABLE();
            return *((Base*)ptr);   // 仅为满足签名: 上面已致命, 正常不会执行
        }
    }
    return *((Base*)ptr);
}

template <class T>
T fromJSValuePointer(JSContext* ctx, JSValueConst value, std::false_type)
{
    return fromJSValueVector<T>(ctx, value, jstype::is_vector<T>());
}

template <class T, std::enable_if_t<!jsbind::is_class_pointer<std::remove_cv_t<std::remove_reference_t<T>>>::value, int>>
T fromJSValue(JSContext* ctx, JSValueConst value)
{
    return fromJSValuePointer<T>(ctx, value, std::is_pointer<T>());
}

// JSRawPointer<Class> 形参提取: 返回 JS 对象内部存储的 wrapper 引用(而非副本),
// 使 cppOwner/cleared 等状态修改能传播回同一个 wrapper(clear 传播语义)。
// 调用方若需独立副本用 auto cp = fromJSValue<JSRawPointer<Class>>(...) (拷贝),
// 若需修改同一 wrapper 用 auto& cp = ... 。
template <class T, std::enable_if_t<jsbind::is_class_pointer<std::remove_cv_t<std::remove_reference_t<T>>>::value, int> = 0>
T& fromJSValue(JSContext* ctx, JSValueConst value)
{
    using CP = std::remove_cv_t<std::remove_reference_t<T>>;
    using Class = typename jsbind::detail::class_pointer_inner<CP>::type;

    JSClassID class_id;
    void* ptr = JS_GetAnyOpaque(value, &class_id);
    if (!ptr)
    {
        JZ_ASSERT_X(0, "fromJSValue<JSRawPointer<Class>&>: 参数不是注册类对象(上层应已校验)");
        return jstype::emptyWrapper<CP>();
    }

    // JSRawPointer<Class> 形参仅支持精确类: 派生类对象传入基类 JSRawPointer 形参时,
    // 无法安全共享底层 JSRawPointer(会造成重复 delete / clear 不传播), 故要求改用 Base* / Base& / Base。
    auto* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    if (engine)
    {
        std::string actual_cid = engine->classCTypeId(class_id);
        std::string want_cid = typeid(Class).name();
        if (actual_cid != want_cid)
        {
            // 已由上层拦下: JZJSEngine::call() 按 CFunctionParam::exactClass 拒绝派生类对象,
            // createArgs 那类地方用 js_check_class_arg(..., true) 校验。这里只兜底报错。
            JZ_ASSERT_X(0, "fromJSValue<JSRawPointer<Class>&>: 形参只接受精确类(上层应已校验)");
            return jstype::emptyWrapper<CP>();
        }
    }

    // 形参要的是"内部包装的引用"(供 setCppOwner / clear 传播), 故必须确认 opaque 就是
    // JSRawPointer<Class> —— JZObject 派生类的包装是 JSObjectPointer, 类型不符时给出明确错误。
    auto* raw = dynamic_cast<jsbind::JSRawPointer<Class>*>((jsbind::JSPointerBase*)ptr);
    if (!raw)
    {
        JZ_ASSERT_X(0, "fromJSValue<JSRawPointer<Class>&>: 包装类型不是 JSRawPointer(上层应已校验)");
        return jstype::emptyWrapper<CP>();
    }
    return *raw;
}

template <>
JSValue fromJSValue(JSContext* ctx, JSValueConst value);

template <>
bool fromJSValue(JSContext* ctx, JSValueConst value);

template <>
int fromJSValue(JSContext* ctx, JSValueConst value);

template <>
double fromJSValue(JSContext* ctx, JSValueConst value);

template <>
std::string fromJSValue(JSContext* ctx, JSValueConst value);

template <>
Json::Value fromJSValue(JSContext* ctx, JSValueConst value);

// to js
JSValue createJSValue(JSContext* ctx, const std::string& ctypeid);
JSValue createJSValue(JSContext* ctx, const std::string& ctypeid, jsbind::JSPointerBase* opaque);

template <class T>
JSValue toJSValue(JSContext* ctx, const T& t);

// JSRawPointer: 按值接收并转移所有权到 JS 对象(底层对象随 JS 对象 GC 释放或保留取决于 cppOwner)。
// 通过移动构造, 调用方的 JSRawPointer 被置空, 不存在重复释放。
template <class Class>
JSValue toJSValue(JSContext* ctx, jsbind::JSRawPointer<Class> t)
{
    // 统一包装: JSRawPointer<Class> 与 Class 共用同一 JS 类(typeid(Class)), opaque 始终是 JSRawPointer<Class>*
    std::string ctypeid = typeid(Class).name();
    return createJSValue(ctx, ctypeid, new jsbind::JSRawPointer<Class>(std::move(t)));
}

// 裸指针 -> JS 对象(经 JSRawPointer 包装): 这是 toJSValue 不支持裸指针时的显式替代。
// cppOwner=true: C++ 持有底层对象, JS 仅持句柄, GC 时不释放;
// cppOwner=false: JS 持有, JS 对象 GC 时 delete 底层对象(须由 new 分配, 勿传栈/外部托管指针)。
// 注意: **每次调用都新建一份包装** —— 同一个 C++ 对象的两份包装不是同一个 JS 对象(`===` 比不得),
// 包装上挂的 JS 状态也各挂各的。控件的事件回调不受影响(它们存在 JZJSWidgetModule 的 C 侧槽里)。
template <class T>
JSValue toJsValueFromPtr(JSContext* ctx, T* ptr, bool cppOwner = false)
{
    return toJSValue(ctx, jsbind::JSRawPointer<T>(ptr, cppOwner));
}

// 返回值转换统一入口: 裸指针走 toJsValueFromPtr(ctx, ptr, cppOwner), 其余走 toJSValue。
// cppOwner 来自注册时记录的返回值所有权(CFunction::returnCppOwner), 不再由这里私自默认。
template <class T>
JSValue toJSValueReturn(JSContext* ctx, T&& t, bool cppOwner)
{
    return toJSValue(ctx, std::forward<T>(t));
}
template <class T>
JSValue toJSValueReturn(JSContext* ctx, T* t, bool cppOwner)
{
    return toJsValueFromPtr(ctx, t, cppOwner);
}

// toJSValue 不支持裸指针: 裸指针须显式经 toJsValueFromPtr 或 jsbind::JSRawPointer<T>(ptr, cppOwner)。
// 此处对指针实参直接 static_assert, 避免误把外部/C++ 持有指针交给 GC 释放。
template <class T>
JSValue toJSValuePointer(JSContext* ctx, const T& t, std::true_type)
{
    static_assert(!std::is_pointer<T>::value,
        "toJSValue 不支持裸指针; 请改用 toJsValueFromPtr(ctx, ptr, cppOwner) 或返回 jsbind::JSRawPointer<T>(ptr, cppOwner)");
    (void)ctx; (void)t;
    return JS_UNDEFINED;
}

template <class T>
JSValue toJSValueVector(JSContext* ctx, const T& vec, std::true_type)
{
    using ValueType = typename jstype::vector_value_type<T>::type;

    JSValue array = JS_NewArray(ctx);
    for (int i = 0; i < (int)vec.size(); i++)
    {
        JSValue value = toJSValue<ValueType>(ctx, vec[i]);
        JS_SetPropertyInt64(ctx, array, i, value);
    }

    return array;
}

template <class T>
JSValue toJSValueVector(JSContext* ctx, const T& t, std::false_type)
{
    using base_type = jstype::remove_cvr_t<T>;

    std::string ctypeid = typeid(base_type).name();

    // 统一包装: 经 create 构造 JSRawPointer<base_type> 包装(默认构造底层对象),
    // 再以赋值写入 t, 与统一前"原位存储"语义一致(仅成员赋值, 不触发拷贝构造)。
    JSValue value = createJSValue(ctx, ctypeid);
    JSClassID class_id;
    // 经多态基类取底层指针(包装类型不确定), 不按具体包装类型强转。
    auto* cpb = (jsbind::JSPointerBase*)JS_GetAnyOpaque(value, &class_id);
    *((base_type*)cpb->getRaw()) = t;
    return value;
}

template <class T>
JSValue toJSValuePointer(JSContext* ctx, const T& t, std::false_type)
{
    return toJSValueVector<T>(ctx, t, jstype::is_vector<T>());
}

template <class T>
JSValue toJSValue(JSContext* ctx, const T& t)
{
    return toJSValuePointer(ctx, t, std::is_pointer<T>());
}

template <>
JSValue toJSValue(JSContext* ctx, const JSValue& value);

template <>
JSValue toJSValue(JSContext* ctx, const bool& value);

template <>
JSValue toJSValue(JSContext* ctx, const int& value);

template <>
JSValue toJSValue(JSContext* ctx, const double& value);

template <>
JSValue toJSValue(JSContext* ctx, const std::string& value);

template <>
JSValue toJSValue(JSContext* ctx, const Json::Value& value);

// checked
bool js_check_ctypeid(JSContext* ctx, JSValueConst js_value, const std::string& ctypeid);

template <class T>
bool js_to_value(JSContext* ctx, JSValueConst js_value, T& value)
{
    if (!js_check_ctypeid(ctx, js_value, typeid(T).name()))
        return false;

    value = fromJSValue<T>(ctx, js_value);
    return true;
}

bool js_has_prop(JSContext* ctx, JSValueConst js_value, const std::string& prop);

template <class T>
bool js_get_prop(JSContext* ctx, JSValueConst js_value, const char* prop, T& value)
{
    JSValue js_prop = JS_GetPropertyStr(ctx, js_value, prop);
    bool ret = true;
    if (!JS_IsUndefined(js_prop))
        ret = js_to_value(ctx, js_prop, value);
    else
        ret = false;

    JS_FreeValue(ctx, js_prop);
    return ret;
}

template <class T>
bool js_get_prop_default(JSContext* ctx, JSValueConst js_value, const char* prop, T& value, const T& default_value)
{
    JSValue js_prop = JS_GetPropertyStr(ctx, js_value, prop);
    bool ret = true;
    if (!JS_IsUndefined(js_prop))
        ret = js_to_value(ctx, js_prop, value);
    else
        value = default_value;

    JS_FreeValue(ctx, js_prop);
    return ret;
}

#endif  // !JS_TYPE_H_
