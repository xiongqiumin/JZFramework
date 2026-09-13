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

class JZJSEngine;   // 前向声明: fromJSValuePointer 统一处理原始类/ClassPointer 时需查询类型集合

// 指针包装类: C++ 侧持有对象的裸指针 + 所有权标记。
// - cppOwner = false: JS 拥有, JS 对象 GC 时由 ClassPointer 析构释放 m_ptr;
// - cppOwner = true : C++ 拥有, JS 仅持句柄, 可通过 clear() 释放句柄(底层对象仍由 C++ 管理)。
// 注意:
// - 仅允许移动, 禁止拷贝(否则两个 cppOwner=false 的实例会重复 delete 同一对象)。
// - 从 JS 取回的 ClassPointer 返回的是对 JS 对象内部 wrapper 的引用(见 fromJSValue 重载),
//   因此 C++ 侧调用 clear() 会直接作用于 JS 持有的那个对象; 之后 JS 再引用即抛异常。
namespace jsbind
{
// 非模板基类: 让检测阶段(不依赖具体 Class)即可判断 ClassPointer 是否已 clear。
class ClassPointerBase
{
public:
    virtual ~ClassPointerBase() = default;
    virtual bool isCleared() const = 0;
    virtual bool cppOwner() const = 0;
    // 返回底层对象裸指针(继承向上转型时, 无需知道具体 Class 模板参数即可取对象指针)
    virtual void* getRaw() const = 0;
};

template <class Class>
class ClassPointer : public ClassPointerBase
{
public:
    ClassPointer(Class* ptr, bool cppOwner)
        : m_ptr(ptr, cppOwner ? &noopDeleter : &defaultDeleter)
        , m_cppOwner(cppOwner)
    {
    }

    Class* get() const { return m_ptr.get(); }
    bool cppOwner() const override { return m_cppOwner; }

    // clear 把共享句柄置空: 所有副本(含 JS 持有的那个)同步看到 nullptr,
    // 因此之后 JS 再引用该对象即触发异常(checkType 阶段)。
    // 仅 C++ 拥有时允许 clear(释放 JS 侧句柄, 底层对象仍由 C++ 管理)。
    void clear()
    {
        assert(m_cppOwner);
        m_ptr = nullptr;
    }

    bool isCleared() const override { return m_ptr == nullptr; }
    void* getRaw() const override { return m_ptr.get(); }

    // 拷贝/移动共享底层对象(shared_ptr), 不会重复释放, 且 clear 通过共享状态传播。
    ClassPointer(const ClassPointer&) = default;
    ClassPointer& operator=(const ClassPointer&) = default;
    ClassPointer(ClassPointer&&) = default;
    ClassPointer& operator=(ClassPointer&&) = default;
    ~ClassPointer() = default;

private:
    static void defaultDeleter(Class* p) { delete p; }
    static void noopDeleter(Class*) {}

    std::shared_ptr<Class> m_ptr;
    bool m_cppOwner;
};

// ClassPointer 萃取: 仅当 T 为 jsbind::ClassPointer<X> 时 type 存在。
namespace detail
{
template <class T>
struct class_pointer_inner
{
};
template <class Class>
struct class_pointer_inner<ClassPointer<Class>>
{
    using type = Class;
};
}  // namespace detail

template <class T>
struct is_class_pointer : std::false_type
{
};
template <class Class>
struct is_class_pointer<ClassPointer<Class>> : std::true_type
{
};
}  // namespace jsbind

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
};

class CProperty
{
public:
    std::string name;

    std::shared_ptr<CFunction> read;
    std::shared_ptr<CFunction> write;
};

class CClassDefine
{
public:
    // 唯一构造入口: JS `new Cls(...)` 透传 (ctx, argc, argv) 生成底层对象并包成 ClassPointer<Class>*。
    // 仅当类无默认构造(如 JZPainter 必须传 widget)或需要构造参数时才用 setCreate 覆盖。
    std::function<void*(JSContext*, int, JSValueConst*)> createArgs;

    std::function<void(void*)> destory;

    // 实例创建后钩子: createValue(type, ptr) 在 JS 对象建好并塞入 opaque 后调用一次
    // (覆盖 new / 返回值转换 / toValuePtr / createCanvas 全部包装路径, 因 toJsValueFromPtr → createJSValue → createValue(type, ptr))。
    // opaque 即 ClassPointer<Class>*; 用于把 JSValue 句柄 / engine 回写到 C++ 包裹对象(如 JSWidgetClass 的 m_jsvalue / m_engine),
    // 供事件转发 callJs 以该对象为 this 调用 JS 方法。由 setInstanceBind 注册。
    std::function<void(JSContext*, JSValue, void* opaque)> onCreated;

    std::string name;
    std::string ctypeid;
    bool isPointer = false;   // true: 该类型对应 ClassPointer<Class>, 检测阶段需判断是否已 clear

    // 继承: 单继承下记录直接基类 ctypeid(供 canConvert 做类型兼容判定)
    std::string superCTypeid;

    std::vector<CProperty> propertys;
    std::vector<std::shared_ptr<CFunction>> functions;
};

// from js
template <class T, std::enable_if_t<!jsbind::is_class_pointer<std::remove_cv_t<std::remove_reference_t<T>>>::value, int> = 0>
T fromJSValue(JSContext* ctx, JSValueConst value);

// T 为裸指针(Class*): 把 JS 对象转成 T*。JS 对象可能是两种表示(与 JZNodeEditor 的统一语义一致):
//   - 原始类(值/引用)对象: 其 opaque 本身就是真实对象指针, 直接转型返回即可;
//   - ClassPointer<Class> 包装类: 其 opaque 是 ClassPointer<Class>*, 取 get() (并判断是否已 clear)。
// null/undefined 映射为空指针, 非对象类型抛 JS TypeError(不静默误转)。
template <class T>
T fromJSValuePointer(JSContext* ctx, JSValueConst value, std::true_type)
{
    static_assert(std::is_pointer<T>::value, "fromJSValuePointer<std::true_type>: 仅支持指针类型");
    using Class = std::remove_pointer_t<T>;

    // null / undefined 直接映射为空指针
    if (JS_IsNull(value) || JS_IsUndefined(value))
        return nullptr;

    // 必须是对象(原始类或 ClassPointer 包装类); 主动判断 value 是否为对象
    JSClassID class_id;
    void* ptr = JS_GetAnyOpaque(value, &class_id);
    if (!ptr)
    {
        JS_ThrowTypeError(ctx, "参数不是对象(Class)");
        throw std::runtime_error("fromJSValue: JSValue 不是 Class 对象");
    }

    // 区分原始类与 ClassPointer 包装类: 后者登记在 m_classPointerTypes 中
    auto* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    if (engine && engine->isClassPointerClassId(class_id))
    {
        auto* cpb = (jsbind::ClassPointerBase*)ptr;
        if (cpb->isCleared())
        {
            JS_ThrowTypeError(ctx, "指针已被释放(clear)，无法访问");
            throw std::runtime_error("ClassPointer 已被 clear");
        }

        // 统一包装模型: 派生类 JS 对象的 opaque 是 ClassPointer<Derived>*, 形参可能要 Base*。
        // 真实类就是 Class 时直接取; 否则复用 JS 原型链判断 is-a(instanceof),
        // 单继承下直接把派生指针当基类指针用(偏移为 0), 无需 C++ upcast。
        std::string actual_cid = engine->classCTypeId(class_id);
        std::string want_cid = typeid(Class).name();
        if (actual_cid == want_cid)
        {
            auto cp = (jsbind::ClassPointer<Class>*)ptr;
            return cp->get();
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
        JS_ThrowTypeError(ctx, "参数类型不匹配: 期望 %s, 实际 %s", want_cid.c_str(), actual_cid.c_str());
        throw std::runtime_error("fromJSValue: 类型不兼容(非基类)");
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

// ClassPointer 在 check type 阶段的 clear 检查(仅对 ClassPointer 类型生效)。
// 用 tag 派发(C++14 兼容, 不使用 if constexpr): true_type 分支做检查并可能抛 JS 异常。
template <class T>
void checkClassPointerCleared(JSContext* ctx, void* ptr, std::true_type)
{
    (void)sizeof(T);  // 保留 T 依赖, 避免未使用模板参数告警
    // 用 ClassPointerBase 安全转型(派生类 JS 对象的 opaque 是 ClassPointer<Derived>*, 无法按 Inner 强转)
    auto* cpb = (jsbind::ClassPointerBase*)ptr;
    if (cpb->isCleared())
    {
        JS_ThrowTypeError(ctx, "指针已被释放(clear)，无法访问");
        throw std::runtime_error("ClassPointer 已被 clear");
    }
}
template <class T>
void checkClassPointerCleared(JSContext*, void*, std::false_type)
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
    checkClassPointerCleared<Base>(ctx, ptr,
        std::integral_constant<bool, jsbind::is_class_pointer<Base>::value>());

    // ClassPointer<Class> 形参已在上面的 is_class_pointer 分支处理(返回 wrapper 本身)。
    // 普通 Class 值/引用形参: 统一模型下其 opaque 始终是 ClassPointer<Base>*, 取底层对象;
    // 值形参返回拷贝, 引用形参返回底层对象的左值引用(生命周期由 JS 对象担保)。
    if (!jsbind::is_class_pointer<Base>::value)
    {
        auto* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
        if (engine && engine->isClassPointerClassId(class_id))
        {
            auto* cpb = (jsbind::ClassPointerBase*)ptr;
            if (cpb->isCleared())
            {
                JS_ThrowTypeError(ctx, "指针已被释放(clear)，无法访问");
                throw std::runtime_error("Class 已被 clear");
            }

            // 真实类就是 Base 直接取; 派生类对象走 JS 原型链判断 is-a 后, 单继承下直接把派生指针当基类指针用
            std::string actual_cid = engine->classCTypeId(class_id);
            std::string want_cid = typeid(Base).name();
            if (actual_cid == want_cid)
            {
                auto cp = (jsbind::ClassPointer<Base>*)ptr;
                return *cp->get();
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
            JS_ThrowTypeError(ctx, "参数类型不匹配: 期望 %s, 实际 %s", want_cid.c_str(), actual_cid.c_str());
            throw std::runtime_error("fromJSValue: 类型不兼容(非基类)");
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

// ClassPointer<Class> 形参提取: 返回 JS 对象内部存储的 wrapper 引用(而非副本),
// 使 cppOwner/cleared 等状态修改能传播回同一个 wrapper(clear 传播语义)。
// 调用方若需独立副本用 auto cp = fromJSValue<ClassPointer<Class>>(...) (拷贝),
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
        JS_ThrowTypeError(ctx, "参数不是对象(ClassPointer)");
        throw std::runtime_error("fromJSValue: JSValue 不是 ClassPointer 对象");
    }

    // ClassPointer<Class> 形参仅支持精确类: 派生类对象传入基类 ClassPointer 形参时,
    // 无法安全共享底层 ClassPointer(会造成重复 delete / clear 不传播), 故要求改用 Base* / Base& / Base。
    auto* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    if (engine)
    {
        std::string actual_cid = engine->classCTypeId(class_id);
        std::string want_cid = typeid(Class).name();
        if (actual_cid != want_cid)
        {
            JS_ThrowTypeError(ctx, "ClassPointer<%s> 形参不支持派生类对象(请用基类指针/引用/值)", want_cid.c_str());
            throw std::runtime_error("fromJSValue: ClassPointer 形参类型不匹配");
        }
    }
    return *((jsbind::ClassPointer<Class>*)ptr);
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
JSValue createJSValue(JSContext* ctx, const std::string& ctypeid, void* opaque);

template <class T>
JSValue toJSValue(JSContext* ctx, const T& t);

// ClassPointer: 按值接收并转移所有权到 JS 对象(底层对象随 JS 对象 GC 释放或保留取决于 cppOwner)。
// 通过移动构造, 调用方的 ClassPointer 被置空, 不存在重复释放。
template <class Class>
JSValue toJSValue(JSContext* ctx, jsbind::ClassPointer<Class> t)
{
    // 统一包装: ClassPointer<Class> 与 Class 共用同一 JS 类(typeid(Class)), opaque 始终是 ClassPointer<Class>*
    std::string ctypeid = typeid(Class).name();
    return createJSValue(ctx, ctypeid, new jsbind::ClassPointer<Class>(std::move(t)));
}

// 裸指针 -> JS 对象(经 ClassPointer 包装): 这是 toJSValue 不支持裸指针时的显式替代。
// cppOwner=true: C++ 持有底层对象, JS 仅持句柄, GC 时不释放;
// cppOwner=false: JS 持有, JS 对象 GC 时 delete 底层对象(须由 new 分配, 勿传栈/外部托管指针)。
template <class T>
JSValue toJsValueFromPtr(JSContext* ctx, T* ptr, bool cppOwner = false)
{
    return toJSValue(ctx, jsbind::ClassPointer<T>(ptr, cppOwner));
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

// toJSValue 不支持裸指针: 裸指针须显式经 toJsValueFromPtr 或 jsbind::ClassPointer<T>(ptr, cppOwner)。
// 此处对指针实参直接 static_assert, 避免误把外部/C++ 持有指针交给 GC 释放。
template <class T>
JSValue toJSValuePointer(JSContext* ctx, const T& t, std::true_type)
{
    static_assert(!std::is_pointer<T>::value,
        "toJSValue 不支持裸指针; 请改用 toJsValueFromPtr(ctx, ptr, cppOwner) 或返回 jsbind::ClassPointer<T>(ptr, cppOwner)");
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

    // 统一包装: 经 create 构造 ClassPointer<base_type> 包装(默认构造底层对象),
    // 再以赋值写入 t, 与统一前"原位存储"语义一致(仅成员赋值, 不触发拷贝构造)。
    JSValue value = createJSValue(ctx, ctypeid);
    JSClassID class_id;
    auto cp = (jsbind::ClassPointer<base_type>*)JS_GetAnyOpaque(value, &class_id);
    *cp->get() = t;
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
