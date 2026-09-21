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

// 从函数签名中提取返回类型 (配合 function_signature_t 使用)
template <typename Sig>
struct function_return;
template <typename R, typename... A>
struct function_return<R(A...)>
{
    using type = R;
};


template <class T>
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
    CFunctionImpl(Func f, bool returnCppOwner = false) : func(f)
    {
        CFunction::returnCppOwner = returnCppOwner;
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
        // 绑定层报错的通行写法是 lambda 里 JS_ThrowXxx(...) 然后直接 return(没有返回值可给),
        // 所以这里必须把"挂着的异常"变成 JS_EXCEPTION 交出去 —— 否则 JS 侧 try/catch 捕不到
        // (异常只是挂在 ctx 上), 而且会污染后续调用。
        if (JS_HasException(ctx))
            return JS_EXCEPTION;
        return JS_UNDEFINED;
    }

    JSValue dealCall(JSContext* ctx, JSValueConst* argv, std::false_type)
    {
        Return ret = callFunction(ctx, argv, make_index_sequence<sizeof...(Args)>());
        if (JS_HasException(ctx))
            return JS_EXCEPTION;   // 同上: lambda 抛了 JS 异常就别再交返回值了
        return toJSValueReturn(ctx, std::move(ret), returnCppOwner);
    }

    template <size_t... idx>
    Return callFunction(JSContext* ctx, JSValueConst* argv, index_sequence<idx...>)
    {
        // 形参是从 argv **借用**转换来的: 值类型/字符串等是拷贝, 形参类型为 JSValue 时就是
        // argv[i] 本身(不 JS_DupValue)。所以 lambda 里不能对 JSValue 形参 JS_FreeValue,
        // 要留存请自己 JS_DupValue(详见 JZJSType.cpp 的 fromJSValue<JSValue>)。
        return func((fromJSValue<remove_cvr_special_t<Args>>(ctx, argv[idx]))...);
    }

    Func func;
};

template <typename Func, typename Return, typename... Args>
CFunctionImpl<Func, Return, Args...>* dealCreateFunctionImpl(Func func, Return (*)(Args...))
{
    return new CFunctionImpl<Func, Return, Args...>(func);
}

template <typename Func>
std::shared_ptr<CFunction> createFunctionImpl(Func f)
{
    using ReturnT = typename function_return<function_signature_t<Func>>::type;    
    static_assert(!std::is_pointer<ReturnT>::value,  "返回值不能是指针, 请在js侧包装");    

    auto cfunc = dealCreateFunctionImpl(f, (function_signature_t<Func>*)nullptr);
    return std::shared_ptr<CFunction>(cfunc);
}

template <typename Return, typename... Args>
std::shared_ptr<CFunction> createFunction(Return (*f)(Args...))
{
    return createFunctionImpl<decltype(f)>(f);
}

template <typename Func>
std::shared_ptr<CFunction> createFunction(Func f)
{
    return createFunctionImpl<decltype(f)>(f);
}

template <typename Return, typename Class, typename... Args>
std::shared_ptr<CFunction> createFunction(Return (Class::*f)(Args...))
{
    auto func = [f](Class* inst, Args... args) -> Return { return (inst->*f)(args...); };
    return createFunctionImpl<decltype(func)>(func);
}

template <typename Return, typename Class, typename... Args>
std::shared_ptr<CFunction> createFunction(Return (Class::*f)(Args...) const)
{
    auto func = [f](Class* inst, Args... args) -> Return { return (inst->*f)(args...); };
    return createFunctionImpl<decltype(func)>(func);
}

template <typename Return, typename Class, typename... Args>
std::shared_ptr<CFunction> createSelfFunction(Class* self, Return (Class::*f)(Args...))
{
    auto func = [self, f](Args... args) -> Return { return (self->*f)(std::forward<Args>(args)...); };
    return createFunctionImpl<decltype(func)>(func);
}

template <typename Return, typename Class, typename... Args>
std::shared_ptr<CFunction> createSelfFunction(Class* self, Return (Class::*f)(Args...) const)
{
    auto func = [self, f](Args... args) -> Return { return (self->*f)(std::forward<Args>(args)...); };
    return createFunctionImpl<decltype(func)>(func);
}

#if __cplusplus >= 201703L
template <typename Return, typename Class, typename... Args>
std::shared_ptr<CFunction> createFunction(Return (Class::*f)(Args...) noexcept)
{
    auto func = [f](Class* inst, Args... args) -> Return { return (inst->*f)(args...); };
    return createFunctionImpl<decltype(func), Extra...>(func);
}

template <typename Return, typename Class, typename... Args>
std::shared_ptr<CFunction> createFunction(Return (Class::*f)(Args...) const noexcept)
{
    auto func = [f](Class* inst, Args... args) -> Return { return (inst->*f)(args...); };
    return createFunctionImpl<decltype(func), Extra...>(func);
}
#endif



template <class Class, std::enable_if_t<!std::is_abstract<Class>::value && std::is_default_constructible<Class>::value, int> = 0>
std::function<void*(JZJSEngine *engine, int, JSValueConst*)> makeDefaultCreate()
{    
    return [](JZJSEngine* ctx, int, JSValueConst*) ->void* {
        return new Class();
    };
}

template <class Class, std::enable_if_t<std::is_abstract<Class>::value, int> = 0>
std::function<void*(JZJSEngine*, int, JSValueConst*)> makeDefaultCreate()
{
    // create 统一约定: 出错"设 JS 异常 + 返回 nullptr", 不抛 C++ 异常(见 JZJSEngine::createValue)。
    return [](JZJSEngine* ctx, int, JSValueConst*) ->void* {
        JS_ThrowInternalError(ctx->context(), "ClassBind: 抽象类不能用默认构造, 需用 setCreate");
        return nullptr;
    };
}
template <class Class, std::enable_if_t<!std::is_abstract<Class>::value && !std::is_default_constructible<Class>::value, int> = 0>
std::function<void*(JZJSEngine*, int, JSValueConst*)> makeDefaultCreate()
{
    // 同上: 无默认构造的类必须自己 setCreate。
    return [](JZJSEngine* ctx, int, JSValueConst*) ->void* {
        JS_ThrowInternalError(ctx->context(), "ClassBind: 类无默认构造, 需用 setCreate");
        return nullptr;
    };
}

template <class Class>
class ClassBind
{
public:
    ClassBind(std::string name)
    {
        m_define.name = name;
        m_define.ctypeid = typeid(Class).name();
        m_define.create = makeDefaultCreate<Class>();
        initOpaqueConvention();
    }

    // 继承: 声明 Class 公开继承 Base(单继承)。
    // 要求 Base 的 ClassBind 已先注册(其 ctypeid 须进入引擎类型表, 否则 canConvert/原型链找不到链)。
    // 注册时只记录直接基类 ctypeid(供 canConvert 类型兼容判定); 单继承下基类子对象偏移为 0, 无需 upcast 函数。
    template <class Base>
    ClassBind(std::string name, const ClassBind<Base>& super)
    {
        static_assert(std::is_base_of<Base, Class>::value, "ClassBind: Class 必须公开继承 Base");
        m_define.name = name;
        m_define.ctypeid = typeid(Class).name();
        m_define.create = makeDefaultCreate<Class>();
        initOpaqueConvention();
        m_define.superCTypeid = super.classDefine().ctypeid;
    }

    // opaque 约定(见 CClassDefine::guardedOpaque):
    //   JZObject 派生类(控件/窗口/菜单/对话框...)的 opaque 是 JZObjectPointer*(守卫) ——
    //     所有权在 JS 侧(JS 对象终结时 delete 底层对象), 但对象也可能先被 C++(父控件)删掉,
    //     守卫能认出"已销毁", 于是这里只删还活着的那个, 天然防重复删除;
    //   其余类(值类型/JZPainter/item)的 opaque 就是裸对象指针, JS 拥有, 终结时 delete。
    // destory 的语义 = "释放 opaque"(不是"释放对象")。
    void initOpaqueConvention()
    {
        if (std::is_base_of<JZObject, Class>::value)
        {
            m_define.guardedOpaque = true;
            m_define.destory = [](void* p) {
                JZObjectPointer* guard = (JZObjectPointer*)p;
                if (!guard)
                    return;
                if (guard->object())
                    delete guard->object();   // 还活着 -> 由建它的 JS 对象收尾(已销毁则父控件删过了)
                delete guard;
            };
        }
        else
        {
            m_define.destory = [](void* p) { delete p; };
        }
    }

    // 自定义构造(无默认构造/需参数/需回写 JS 句柄的类): 覆盖默认工厂。
    // 签名 (ctx, argc, argv): 仅负责造底层对象并包成 JSRawPointer<Class>*, 句柄回写交 setInstanceBind 注册的 onCreated。
    void setCreate(std::function<void*(JZJSEngine*, int, JSValueConst*)> create) { m_define.create = std::move(create); }

    // 声明"指针语义": 该类型的包装登记进引擎的身份表(同底层对象 ⇒ 同 JS 对象, `===` 才有意义),
    // 并支持 moveToCpp/moveToJs 的句柄托管。**默认是值语义**, 值类型(JZPoint/JZSize/JZRect/
    // JZColor/JZFont/JZImage)别调这个。加不加都不影响 cppOwner/clear 的既有行为。
    void setPointerSemantics() { m_define.pointerSemantics = true; }

    template <typename Return, typename... Args, typename... Extra>
    CFunction *def(std::string name, Return (Class::*f)(Args...), Extra... extra)
    {
        std::shared_ptr<CFunction> impl = createFunction(f, extra...);
        impl->name = name;
        m_define.functions.push_back(impl);
        return m_define.functions.back().get();
    }

    template <typename Return, typename... Args, typename... Extra>
    CFunction *def(std::string name, Return (Class::*f)(Args...) const, Extra... extra)
    {
        std::shared_ptr<CFunction> impl = createFunction(f, extra...);
        impl->name = name;
        m_define.functions.push_back(impl);
        return m_define.functions.back().get();
    }

    template <typename Func, typename... Extra>
    CFunction *def(std::string name, Func f, Extra... extra)
    {
        std::shared_ptr<CFunction> impl = createFunction(f, extra...);
        impl->name = name;
        m_define.functions.push_back(impl);
        return m_define.functions.back().get();
    }

    // ---- 直接挂 quickjs C 函数 ----
    //   JSValue fn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
    // 与上面几个 def 的区别: **不生成形参类型表, 也不经过引擎的 magic 分发** —— quickjs 以
    // JS_CFUNC_generic 原样调用它, 因此它能拿到 this_val 与原始 argv(自己校验、自己报错)。
    // 需要 this_val 的接口只能走这条路: def 的 lambda 只拿得到"已经转好的形参"。
    // 例: bind_w.def("setParentWidget", js_JZWidget_setParentWidget, 1);
    // 注意: 这两个是非模板重载, 传 C 函数名/函数指针时优先命中它们(而不是上面的通用模板);
    //       传 lambda 时仍然走模板(没有到 JSCFunction* 的隐式转换)。
    void def(std::string name, JSCFunction* fn)
    {
        def(name, fn, 0);
    }
    
    void def(std::string name, JSCFunction* fn, int length)
    {
        CRawFunction raw;
        raw.name = std::move(name);
        raw.function = fn;
        raw.length = length;   // 仅 JS 侧 Function.length 的展示值, 不做校验
        m_define.rawFunctions.push_back(std::move(raw));
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
