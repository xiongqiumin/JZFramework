#ifndef JS_BIND_H_
#define JS_BIND_H_

#include <type_traits>

#include "JZJSType.h"
#include "quickjs/quickjs.h"

namespace jsbind
{
// copy from pybind11
template <size_t...>
struct index_sequence
{
};
template <size_t N, size_t... S>
struct make_index_sequence_impl : make_index_sequence_impl<N - 1, N - 1, S...>
{
};
template <size_t... S>
struct make_index_sequence_impl<0, S...>
{
    using type = index_sequence<S...>;
};
template <size_t N>
using make_index_sequence = typename make_index_sequence_impl<N>::type;

template <bool B, typename T = void>
using enable_if_t = typename std::enable_if<B, T>::type;
template <bool B, typename T, typename F>
using conditional_t = typename std::conditional<B, T, F>::type;
template <typename T>
using remove_cv_t = typename std::remove_cv<T>::type;
template <typename T>
using remove_reference_t = typename std::remove_reference<T>::type;
template <typename T>
using remove_cvr_t = remove_cv_t<remove_reference_t<T>>;

// 辅助判断：T 剥离 CVR 后是否为 std::string 或 jstring
template <typename T>
struct is_special_type
{
private:
    // 剥离 T 的引用、const、volatile，得到裸类型
    using raw_type = std::remove_cv_t<std::remove_reference_t<T>>;

public:
    // 判断裸类型是否为目标类型（std::string 或 Json::Value）
    static constexpr bool value = !std::is_pointer<T>::value && (std::is_same<raw_type, std::string>::value 
        || std::is_same<raw_type, Json::Value>::value);
};

// 主模板：对特殊类型（string/Json::value）移除 CVR，其他类型保持不变
template <typename T>
struct remove_cvr_special
{
    // 条件选择：若为特殊类型则返回剥离后的类型，否则返回原类型
    using type = std::conditional_t<is_special_type<T>::value,
                                    std::remove_cv_t<std::remove_reference_t<T>>,  // 移除 CVR
                                    T                                              // 保持原类型
                                    >;
};

// 模板别名（简化使用，类似标准库的 _t 后缀风格）
template <typename T>
using remove_cvr_special_t = typename remove_cvr_special<T>::type;

/// Strip the class from a method type
template <typename T>
struct remove_class
{
};
template <typename C, typename R, typename... A>
struct remove_class<R (C::*)(A...)>
{
    using type = R(A...);
};
template <typename C, typename R, typename... A>
struct remove_class<R (C::*)(A...) const>
{
    using type = R(A...);
};

template <typename F>
struct strip_function_object
{
    // If you are encountering an
    // 'error: name followed by "::" must be a class or namespace name'
    // with the Intel compiler and a noexcept function here,
    // try to use noexcept(true) instead of plain noexcept.
    using type = typename remove_class<decltype(&F::operator())>::type;
};

// Extracts the function signature from a function, function pointer or lambda.
template <typename Function, typename F = remove_reference_t<Function>>
using function_signature_t =
    conditional_t<std::is_function<F>::value, F,
                  typename conditional_t<std::is_pointer<F>::value || std::is_member_pointer<F>::value,
                                         std::remove_pointer<F>, strip_function_object<F>>::type>;

// class function
template <class T>
void* createClass()
{
    return new T();
}
template <class T>
void destoryClass(void* ptr)
{
    delete (T*)ptr;
}

// 统一包装模型下, ClassPointer<Class> 与 Class 共用同一个 JS 类(typeid(Class)),
// 因此形参/返回值类型推导时, 把 ClassPointer<Class> 映射成 typeid(Class)(指针/引用再加 "*")。
template <class T, class Enable = void>
struct ctypeid_of
{
    static std::string get()
    {
        std::string base_type = typeid(typename std::remove_pointer<T>::type).name();
        if (std::is_pointer<T>::value || std::is_reference<T>::value)
            base_type += "*";
        return base_type;
    }
};

template <class T>
struct ctypeid_of<T, enable_if_t<is_class_pointer<remove_cvr_t<T>>::value>>
{
    static std::string get()
    {
        using Class = typename detail::class_pointer_inner<remove_cvr_t<T>>::type;
        std::string base_type = typeid(Class).name();
        if (std::is_pointer<T>::value || std::is_reference<T>::value)
            base_type += "*";
        return base_type;
    }
};

// CFunctionImpl
template <class T>
std::string getCTypeid()
{
    return ctypeid_of<T>::get();
}

template <class type>
void getFunctionParam(std::vector<CFunctionParam>&)
{
}

template <class type, typename T, typename... Args>
void getFunctionParam(std::vector<CFunctionParam>& list)
{
    CFunctionParam param;
    param.type = -1;
    param.ctypeid = getCTypeid<T>();

    list.push_back(param);
    getFunctionParam<type, Args...>(list);
}

template <typename Func, typename Return, typename... Args>
class CFunctionImpl : public CFunction
{
public:
    CFunctionImpl(Func f) : func(f)
    {
        result.ctypeid = getCTypeid<Return>();
        if (result.ctypeid == typeid(void).name())
            result.ctypeid.clear();

        getFunctionParam<int, Args...>(params);
    }

    virtual ~CFunctionImpl()
    {
    }

    virtual JSValue call(JSContext* ctx, JSValueConst* argv)
    {
        return dealCall(ctx, argv, std::is_void<Return>());
    }

    JSValue dealCall(JSContext* ctx, JSValueConst* argv, std::true_type)
    {
        callFunction(ctx, argv, make_index_sequence<sizeof...(Args)>());
        return JS_UNDEFINED;
    }

    JSValue dealCall(JSContext* ctx, JSValueConst* argv, std::false_type)
    {
        Return ret = callFunction(ctx, argv, make_index_sequence<sizeof...(Args)>());
        return toJSValue(ctx, std::move(ret));
    }

    template <size_t... idx>
    Return callFunction(JSContext* ctx, JSValueConst* argv, index_sequence<idx...>)
    {
        return func((fromJSValue<remove_cvr_special_t<Args>>(ctx, argv[idx]))...);
    }

    Func func;
};

template <typename Func, typename Return, typename... Args>
CFunctionImpl<Func, Return, Args...>* dealCreateFunctionImpl(Func func, Return (*)(Args...))
{
    return new CFunctionImpl<Func, Return, Args...>(func);
}

template <typename Func, typename... Extra>
std::shared_ptr<CFunction> createFunctionImpl(Func f, Extra... extra)
{
    auto cfunc = dealCreateFunctionImpl(f, (function_signature_t<Func>*)nullptr);
    return std::shared_ptr<CFunction>(cfunc);
}

template <typename Return, typename... Args, typename... Extra>
std::shared_ptr<CFunction> createFunction(Return (*f)(Args...), Extra... extra)
{
    return createFunctionImpl<decltype(f), Extra...>(f, extra...);
}

template <typename Func, typename... Extra>
std::shared_ptr<CFunction> createFunction(Func f, Extra... extra)
{
    return createFunctionImpl<decltype(f), Extra...>(f, extra...);
}

template <typename Return, typename Class, typename... Args, typename... Extra>
std::shared_ptr<CFunction> createFunction(Return (Class::*f)(Args...), Extra... extra)
{
    auto func = [f](Class* inst, Args... args) -> Return { return (inst->*f)(args...); };
    return createFunctionImpl<decltype(func), Extra...>(func, extra...);
}

template <typename Return, typename Class, typename... Args, typename... Extra>
std::shared_ptr<CFunction> createFunction(Return (Class::*f)(Args...) const, Extra... extra)
{
    auto func = [f](Class* inst, Args... args) -> Return { return (inst->*f)(args...); };
    return createFunctionImpl<decltype(func), Extra...>(func, extra...);
}

template <typename Return, typename Class, typename... Args, typename... Extra>
std::shared_ptr<CFunction> createSelfFunction(Class* self, Return (Class::*f)(Args...), Extra... extra)
{
    auto func = [self, f](Args... args) -> Return { return (self->*f)(std::forward<Args>(args)...); };
    return createFunctionImpl<decltype(func), Extra...>(func, extra...);
}

template <typename Return, typename Class, typename... Args, typename... Extra>
std::shared_ptr<CFunction> createSelfFunction(Class* self, Return (Class::*f)(Args...) const, Extra... extra)
{
    auto func = [self, f](Args... args) -> Return { return (self->*f)(std::forward<Args>(args)...); };
    return createFunctionImpl<decltype(func), Extra...>(func, extra...);
}

#if __cplusplus >= 201703L
template <typename Return, typename Class, typename... Args, typename... Extra>
std::shared_ptr<CFunction> createFunction(Return (Class::*f)(Args...) noexcept, Extra... extra)
{
    auto func = [f](Class* inst, Args... args) -> Return { return (inst->*f)(args...); };
    return createFunctionImpl<decltype(func), Extra...>(func, extra...);
}

template <typename Return, typename Class, typename... Args, typename... Extra>
std::shared_ptr<CFunction> createFunction(Return (Class::*f)(Args...) const noexcept, Extra... extra)
{
    auto func = [f](Class* inst, Args... args) -> Return { return (inst->*f)(args...); };
    return createFunctionImpl<decltype(func), Extra...>(func, extra...);
}
#endif

// 默认构造工厂: 抽象类无法 new, 该类用 setCreate 提供具体构造; 否则运行期抛异常。
// 用 is_abstract 分派, 避免对抽象类实例化 `new Class()` 导致编译失败。
template <class Class, std::enable_if_t<!std::is_abstract<Class>::value, int> = 0>
std::function<void*()> makeDefaultCreate()
{
    return []() { return new ClassPointer<Class>(new Class(), false); };
}
template <class Class, std::enable_if_t<std::is_abstract<Class>::value, int> = 0>
std::function<void*()> makeDefaultCreate()
{
    return []() { throw std::runtime_error("ClassBind: 抽象类不能用默认构造, 需用 setCreate"); return (void*)nullptr; };
}

template <class Class>
class ClassBind
{
public:
    ClassBind(std::string name)
    {
        m_define.name = name;
        m_define.ctypeid = typeid(Class).name();
        // 统一包装模型: 值/引用类对象 opaque 同样存放 ClassPointer<Class>*, 由 JS 管理 wrapper;
        // 底层对象所有权按 cppOwner 决定(默认 false = JS 拥有并随 GC 释放), 与 ClassPointer 一致。
        m_define.isPointer = true;
        m_define.create = makeDefaultCreate<Class>();
        m_define.destory = [](void* p) { delete (jsbind::ClassPointer<Class>*)p; };
    }

    // 自定义构造(用于无默认构造函数的类, 如事件/抽象基类): 改用指定工厂创建 JS 实例。
    void setCreate(std::function<void*()> create) { m_define.create = create; }

    template <typename Return, typename... Args, typename... Extra>
    void def(std::string name, Return (Class::*f)(Args...), Extra... extra)
    {
        std::shared_ptr<CFunction> impl = createFunction(f, extra...);
        impl->name = name;
        m_define.functions.push_back(impl);
    }

    template <typename Return, typename... Args, typename... Extra>
    void def(std::string name, Return (Class::*f)(Args...) const, Extra... extra)
    {
        std::shared_ptr<CFunction> impl = createFunction(f, extra...);
        impl->name = name;
        m_define.functions.push_back(impl);
    }

    template <typename Func, typename... Extra>
    void def(std::string name, Func f, Extra... extra)
    {
        std::shared_ptr<CFunction> impl = createFunction(f, extra...);
        impl->name = name;
        m_define.functions.push_back(impl);
    }

    template <typename PropertyType>
    void defProperty(std::string name, PropertyType Class::* member)
    {
        auto func_read = [member](Class* obj) -> PropertyType { return obj->*member; };
        auto func_write = [member](Class* obj, PropertyType v) { obj->*member = v; };

        defProperty<PropertyType>(name, func_read, func_write);
    }

    template <typename PropertyType>
    void defProperty(std::string name, std::function<PropertyType(Class*)> read,
                     std::function<void(Class*, PropertyType)> write)
    {
        CProperty prop;
        prop.name = name;
        prop.read = createFunction(read);
        if (write)
            prop.write = createFunction(write);
        m_define.propertys.push_back(prop);
    }

    const CClassDefine& classDefine() const
    {
        return m_define;
    }

    CClassDefine m_define;
};

}  // namespace jsbind

#endif  // ! JS_BIND_H_
