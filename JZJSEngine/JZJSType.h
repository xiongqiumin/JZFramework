#ifndef JS_TYPE_H_
#define JS_TYPE_H_

#include <assert.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "json/json.h"
#include "quickjs/quickjs.h"

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
    constexpr bool is_ref = std::is_reference<T>::Value;
    // 2. 剥离引用后，获取原始类型（去 const 可选）
    using raw_type = std::remove_reference_t<T>;
    using unqualified_type = std::remove_const_t<raw_type>;  // 忽略 const 修饰
                                                             // 3. 判断原始类型是否为 U
    constexpr bool is_target_type = std::is_same<unqualified_type, U>::Value;

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
    std::function<void*()> create;
    std::function<void(void*)> destory;

    std::string name;
    std::string ctypeid;

    std::vector<CProperty> propertys;
    std::vector<std::shared_ptr<CFunction>> functions;
};

// from js
template <class T>
T fromJSValue(JSContext* ctx, JSValueConst value);

template <class T>
T fromJSValuePointer(JSContext* ctx, JSValueConst value, std::true_type)
{
    static_assert(std::is_class<std::remove_pointer_t<T>>(), "only support class pointer");

    JSClassID class_id;
    auto ptr = JS_GetAnyOpaque(value, &class_id);
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

template <class T>
T fromJSValueVector(JSContext* ctx, JSValueConst value, std::false_type)
{
    // string 和 Json::Value 是值语义, 所以不能使用引用
    jstype::check_forbid_reference<T, std::string>();
    jstype::check_forbid_reference<T, Json::Value>();

    JSClassID class_id;
    auto ptr = JS_GetAnyOpaque(value, &class_id);
    return *((jstype::remove_cvr_t<T>*)ptr);
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

template <class T>
JSValue toJSValue(JSContext* ctx, const T& t);

template <class T>
JSValue toJSValuePointer(JSContext* ctx, const T& t, std::true_type)
{
    assert(0);
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
    base_type* ptr = (base_type*)JS_GetAnyOpaque(value, &class_id);
    *ptr = t;

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
