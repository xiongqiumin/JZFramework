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
    JSType_null,
    JSType_undefined,
    JSType_bool,
    JSType_int,
    JSType_double,
    JSType_string,
    JSType_jsonValue,

    JSType_object = 1000,

    // 透明控件类的实例统一按"对象指针"上报: 控件族对引擎透明(共用一个 op class, 没有各自的类型号),
    // 所以 jsValueType 对 op class 的实例回这个类型, 而控件方法的**对象形参**声明成
    // `JZObjectPointer*`(见 initTypeMap 的注册 / JZJSWidgetModule::widgetFromThis)。
    // 值取 1100: 要 > JSType_object(才能走 checkFunctionArg 的"对象"校验分支),
    // 又要 < JSType_objectUser(那是 m_objectId 给注册类分配的起点)。
    JSType_objectPointer = 1100,

    JSType_objectUser = 2000,
};

struct JZFRAMEWORK_API CFunctionParam
{
    CFunctionParam();

    int type = JSType_none;
    std::string name;
    std::string ctypeid;
    bool allowNull = false;
};

class JZFRAMEWORK_API CFunction
{
public:
    CFunction();
    virtual ~CFunction();

    virtual JSValue call(JSContext* ctx, JSValueConst* argv) = 0;

    std::string name;
    CFunctionParam result;
    std::vector<CFunctionParam> params;
    bool returnCppOwner = false;   // 返回值所有权: 注册返回裸指针的函数时记录; false=JS 拥有(GC 时 delete), true=C++ 持有(JS 不释放)
    bool memberFunction = false;
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
struct JZFRAMEWORK_API CRawFunction
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
struct JZFRAMEWORK_API CDataFunction
{
    std::string name;
    JSCFunctionData* function = nullptr;
    int length = 0;               // JS 侧 Function.length(仅展示)
    int magic = 0;                // 原样透传给 C 函数
    std::vector<JSValue> data;    // 原样透传给 C 函数(注册时 quickjs 会对每个元素 dup 一份)
};

// 指针语义类(JZObject 派生)的 opaque 约定: 存 **JZObjectPointer*** (JZCore 现成的共享守卫的堆副本),
// 而不是底层对象裸指针。为什么:
//   ① 所有权一直在 JS 侧(对象由建它的 JS 对象负责 delete), 但底层对象也可能**先被 C++ 侧删掉**
//      (控件挂进对象树后由父控件 delete)。裸指针这时就是 UAF、GC 时再 delete 就是 double free;
//      JZObjectPointer 由 ~JZObject 置空, 于是"已销毁"能被认出来(调用抛异常、终结时跳过 delete)。
//   ② 不引入所有权翻转: 谁删都可能, 守卫决定"还该不该删"。
// 释放: JZClassDefine::destory 负责 —— object() 还有就 delete 对象, 再 delete 守卫本身。
class CClassDefine
{
public:
    // 唯一构造入口: JS `new Cls(...)` 透传 (ctx, argc, argv) 生成底层对象并包成 JSRawPointer<Class>*。
    // 仅当类无默认构造(如 JZPainter 必须传 widget)或需要构造参数时才用 setCreate 覆盖。
    std::function<void*(JZJSEngine*, int, JSValueConst*)> create;
    std::function<void(void*)> destory;

    // opaque 是 JZObjectPointer*(JZObject 派生类: 控件/窗口/菜单/对话框...), 而不是裸对象指针。
    // 由 ClassBind 按 `Class 是否派生自 JZObject` 自动置位; 非 JZObject 的类(值类型/JZPainter/item)保持裸指针。
    bool guardedOpaque = false;

    std::string name;
    std::string ctypeid;

    // 语义: **默认值语义**(每次包装独立, 不做同一性/句柄管理)。
    // 显式声明为"指针语义"(有身份的多态对象: 控件/布局/事件/item/painter...)才进引擎的身份表,
    // 于是 ① 同底层对象 ⇒ 同 JS 对象(`===` 才有意义) ② 所有权翻转/失效在所有包装间同步。
    // 值语义类型(JZPoint/JZSize/JZRect/JZColor/JZFont/JZImage)保持 false, 判等用各自的 equal()。
    // 注册处: ClassBind::setPointerSemantics()。
    bool pointerSemantics = false;

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
template <class T>
T fromJSValue(JSContext* ctx, JSValueConst value);

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

template <class T>
T fromJSValuePointer(JSContext* ctx, JSValueConst value, std::true_type)
{
    static_assert(std::is_pointer<T>::value, "fromJSValuePointer<std::true_type>: 仅支持指针类型");
    using Class = std::remove_pointer_t<T>;

    // null / undefined 直接映射为空指针
    if (JS_IsNull(value) || JS_IsUndefined(value))
        return nullptr;

    // 必须是对象(注册类对象); 主动判断 value 是否为对象
    JSClassID class_id;
    void* ptr = JS_GetAnyOpaque(value, &class_id);
    if (!ptr)
    {
        JZ_ASSERT_X(0, "fromJSValue<Class*>: 参数不是注册类对象(上层应已校验)");
        return nullptr;
    }

    // 指针语义类: opaque 是 JZObjectPointer(守卫) -> 解出底层对象; 对象已被删掉时抛异常(不是 UAF)
    auto* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    return (T)engine->objectFromOpaque((int)class_id, ptr);
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

template <class T>
T fromJSValueVector(JSContext* ctx, JSValueConst value, std::false_type)
{
    // string 和 Json::Value 是值语义, 所以不能使用引用
    jstype::check_forbid_reference<T, std::string>();
    jstype::check_forbid_reference<T, Json::Value>();

    using RawType = std::remove_cv_t<std::remove_reference_t<T>>;

    // 值语义类对象形参(JZFont / JZColor / JZRect / JZPen ...): opaque 就是对象本身(裸指针),
    // 按形参类型(值 / const 引用)直接取出。
    JSClassID class_id = 0;
    auto* engine = (JZJSEngine*)JS_GetContextOpaque(ctx);
    void* ptr = engine->objectFromOpaque((int)JS_GetClassID(value), JS_GetAnyOpaque(value, &class_id));
    JZ_ASSERT_X(ptr, "fromJSValue: 参数不是注册类对象(上层应已校验)");
    return *((RawType*)ptr);
}

template <class T>
T fromJSValuePointer(JSContext* ctx, JSValueConst value, std::false_type)
{
    return fromJSValueVector<T>(ctx, value, jstype::is_vector<T>());
}

template <class T>
T fromJSValue(JSContext* ctx, JSValueConst value)
{
    return fromJSValuePointer<T>(ctx, value, std::is_pointer<T>());
}

// ⚠ 下面这些是**模板显式特化**, 定义在 JZJSType.cpp 里(不是 header-only) ⇒ 动态库模式下
//    必须带 JZFRAMEWORK_API, 否则使用方(fromJSValue<T>/toJSValue<T> 的调用点)链接期 LNK2019。
template <>
JZFRAMEWORK_API JSValue fromJSValue(JSContext* ctx, JSValueConst value);

template <>
JZFRAMEWORK_API bool fromJSValue(JSContext* ctx, JSValueConst value);

template <>
JZFRAMEWORK_API int fromJSValue(JSContext* ctx, JSValueConst value);

template <>
JZFRAMEWORK_API double fromJSValue(JSContext* ctx, JSValueConst value);

template <>
JZFRAMEWORK_API std::string fromJSValue(JSContext* ctx, JSValueConst value);

template <>
JZFRAMEWORK_API Json::Value fromJSValue(JSContext* ctx, JSValueConst value);

// 对象指针形参(控件方法的宿主/对象形参): 取 opaque 里那份 JZObjectPointer —— **借用**。
// 类型号 JSType_objectPointer 由 initTypeMap 注册, "是否活着的对象指针"由 checkFunctionArg 校验。
class JZObjectPointer;
template <>
JZFRAMEWORK_API JZObjectPointer* fromJSValue(JSContext* ctx, JSValueConst value);

// to js
JZFRAMEWORK_API JSValue createJSValue(JSContext* ctx, const std::string& ctypeid);

template <class T>
JSValue toJSValue(JSContext* ctx, const T& t);

template <class T>
JSValue toJsValueFromPtr(JSContext* ctx, T* ptr, bool cppOwner);

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
    // 空指针 = "C++ 侧没有这个对象" -> 交给 JS 真 null(与 JS 语义一致: `if (p)` / `p === null` 判得出空)。
    // 不能走 toJsValueFromPtr: 它会去包一个空指针(包装的守卫指针构造要解引用底层对象) —— 直接崩。
    if (!t)
        return JS_NULL;
    return toJsValueFromPtr(ctx, t, cppOwner);
}

// toJSValue 不支持裸指针: 裸指针须显式经 toJsValueFromPtr 或 jsbind::JSRawPointer<T>(ptr, cppOwner)。
// 此处对指针实参直接 static_assert, 避免误把外部/C++ 持有指针交给 GC 释放。
template <class T>
JSValue toJSValuePointer(JSContext* ctx, const T& t, std::true_type)
{
    static_assert(!std::is_pointer<T>::value,
        "toJSValue 不支持裸指针; 请改用 toJsValueFromPtr(ctx, ptr, cppOwner)");
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

    JSValue value = createJSValue(ctx, ctypeid);
    JSClassID class_id;
    auto* cpb = (T*)JS_GetAnyOpaque(value, &class_id);
    *((base_type*)cpb) = t;
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
JZFRAMEWORK_API JSValue toJSValue(JSContext* ctx, const JSValue& value);

template <>
JZFRAMEWORK_API JSValue toJSValue(JSContext* ctx, const bool& value);

template <>
JZFRAMEWORK_API JSValue toJSValue(JSContext* ctx, const int& value);

template <>
JZFRAMEWORK_API JSValue toJSValue(JSContext* ctx, const double& value);

template <>
JZFRAMEWORK_API JSValue toJSValue(JSContext* ctx, const std::string& value);

template <>
JZFRAMEWORK_API JSValue toJSValue(JSContext* ctx, const Json::Value& value);

// checked
JZFRAMEWORK_API bool js_check_ctypeid(JSContext* ctx, JSValueConst js_value, const std::string& ctypeid);

template <class T>
bool js_to_value(JSContext* ctx, JSValueConst js_value, T& value)
{
    if (!js_check_ctypeid(ctx, js_value, typeid(T).name()))
        return false;

    value = fromJSValue<T>(ctx, js_value);
    return true;
}

JZFRAMEWORK_API bool js_has_prop(JSContext* ctx, JSValueConst js_value, const std::string& prop);

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
