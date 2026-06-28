#ifndef JZ_SIGNAL_H_
#define JZ_SIGNAL_H_

#include <memory>
#include <vector>
#include <map>
#include <functional>
#include <type_traits>
#include <tuple>
#include "JZEvent.h"
#include "JZVariant.h"
#include "JZObjectPointer.h"

#ifdef emit
#undef emit
#endif //emit

enum JZConnectType
{    
    AutoConnection,
    DirectConnection,
    QueuedConnection,
    BlockingQueuedConnection,
};

class JZObject;
//JZSignalEvent
class JZSignalEvent : public JZEvent
{
public:
    JZSignalEvent();
    ~JZSignalEvent();

    JZObjectPointer sender;
    JZObjectPointer recv;
    std::function<void(JZObject*, const JZVariantList&)> slot;
    std::function<void(const JZVariantList&)> lambdaSlot;
    JZVariantList args;
};

//JZSLot
template<typename... Args>
class JZSlot
{
public:
    std::function<void(JZObject*, Args...)> func;
    std::function<void(Args...)> lambdaFunc;
    
    std::function<void(JZVariantList&,Args...)> packFunc;
    std::function<void(JZObject*, const JZVariantList&)> asyncFunc;
    std::function<void(const JZVariantList&)> asyncLambdaFunc;
};

//JZConnect
template<typename... Args>
class JZConnect
{
public:
    int id;
    JZConnectType type;
    bool hasRecv;          //是否拥有接受者
    JZObjectPointer recv;  //接受者
    JZSlot<Args...> slot;
};

class JZBaseSignal
{
public:
    JZBaseSignal();
    virtual ~JZBaseSignal();
    
    void pushSender(JZObjectPointer &recv, JZObject *sender);
    void popSender(JZObjectPointer &recv);

    JZObject *currentThread();
    bool isCurrentThread(JZObject *recv); 

    bool isBlockSingals(JZObject *object);
    JZObjectPointer guardPointer(JZObject *obj);
    void postEvent(JZObject *recvier, JZSignalEvent *event);
    void postEventAndWait(JZObject *recvier, JZSignalEvent *event);
};

template<typename... Args>
class JZSignal : public JZBaseSignal
{
public:        
    JZSignal(JZObject *object)
    {
        m_object = object;
        m_connectId = 0;
    }
    virtual ~JZSignal() {

    }

    int connectCount()
    {
        return (int)m_connects.size();
    }

    int addConnect(JZConnect<Args...> connect)
    {
        connect.id = m_connectId++;
        m_connects.push_back(connect);
        return connect.id;
    }

    void clearConnect()
    {
        m_connects.clear();
    }

    void removeById(int id)
    {
        for (int i = (int)m_connects.size() - 1; i >= 0; i--)
        {
            if (m_connects[i].id == id) {
                m_connects.erase(m_connects.begin() + i);
            }
        }
    }

    void removeRecvObject(JZObject *object)
    {
        for (int i = (int)m_connects.size() - 1; i >= 0; --i)
        {
            if (m_connects[i].recv == object) {
                m_connects.erase(m_connects.begin() + i);
            }
        }
    }

    void emit(Args... args)
    {
        if (isBlockSingals(m_object))
            return;

        std::vector<JZConnect<Args...>> connect_list = m_connects;
        for (int i = 0; i < (int)connect_list.size(); i++)
        {
            auto& conn = connect_list[i];
            if (conn.hasRecv && !conn.recv)
                continue;

            JZObject *sender = m_object;
            JZObject *recvier = conn.recv.object();
            int type = conn.type;

            if ((type == AutoConnection && isCurrentThread(recvier))
                || type == DirectConnection)
            {
                pushSender(conn.recv,sender);
                if(conn.slot.func)
                    conn.slot.func(recvier, args...);
                else
                    conn.slot.lambdaFunc(args...);
                popSender(conn.recv);
            }
            else
            {                
                if (type == AutoConnection || type == QueuedConnection || type == BlockingQueuedConnection)
                {
                    JZ_ASSERT(conn.slot.packFunc);

                    JZSignalEvent *event = new JZSignalEvent();
                    event->sender = guardPointer(sender);
                    event->recv = conn.recv;
                    event->slot = conn.slot.asyncFunc;
                    event->lambdaSlot = conn.slot.asyncLambdaFunc;
                    conn.slot.packFunc(event->args, args...);

                    JZObject *recv_object = event->recv.object();
                    if (type == AutoConnection || type == QueuedConnection)
                        postEvent(recv_object, event);
                    else
                        postEventAndWait(recv_object, event);
                }
                else
                {
                    JZ_ASSERT(0);
                }
            }
        }
    }

protected:
    std::vector<JZConnect<Args...>> m_connects;

    JZObject *m_object;
    int m_connectId;
};

typedef JZSignal<> JZSigVoid;
typedef JZSignal<bool> JZSigBool;
typedef JZSignal<int> JZSigInt;
typedef JZSignal<double> JZSigDouble;
typedef JZSignal<const std::string&> JZSigString;
typedef JZSignal<const JZByteArray&> JZSigByteArray;

namespace jzsignal 
{

template <typename T>
using remove_cvr_t = typename std::remove_cv<typename std::remove_reference<T>::type>::type;

// 辅助：萃取 函数参数类型
template<typename F>
struct function_traits;

template<typename R, typename... Args>
struct function_traits<R(*)(Args...)> {
    using args_tuple = std::tuple<Args...>;
};

template<typename R, typename C, typename... Args>
struct function_traits<R(C::*)(Args...)> {
    using class_type = C;
    using args_tuple = std::tuple<Args...>;
};

template<typename R, typename C, typename... Args>
struct function_traits<R(C::*)(Args...) const> {
    using class_type = C;
    using args_tuple = std::tuple<Args...>;
};

template<typename Lambda>
struct function_traits : function_traits<decltype(&Lambda::operator())> {};

/// Strip the class from a method type
template <typename T> struct remove_class {};
template <typename C, typename R, typename... A> struct remove_class<R(C::*)(A...)> { using type = R(A...); };
template <typename C, typename R, typename... A> struct remove_class<R(C::*)(A...) const> { using type = R(A...); };

template <typename F> struct strip_function_object {
    // If you are encountering an
    // 'error: name followed by "::" must be a class or namespace name'
    // with the Intel compiler and a noexcept function here,
    // try to use noexcept(true) instead of plain noexcept.
    using type = typename remove_class<decltype(&F::operator())>::type;
};

//is_inherits
template<typename F>
using function_class_t = typename function_traits<F>::class_type;

// 核心：检测 Derived 是否继承自 成员函数所属类（基于整合后的 function_traits）
template<typename Derived, typename Func>
struct is_inherits_t {
    // 从 Func 中萃取所属类（Base）
    using Base = function_class_t<Func>;
    // 检测继承关系：Base 是基类，Derived 是待检测的派生类
    static constexpr bool value = std::is_base_of<Base, Derived>::value;
};

template<typename Derived, typename Func>
constexpr bool is_inherits_v = is_inherits_t<Derived, Func>::value;

//IsAllDefined
template <typename... Args>
struct IsAllDefined;

template <>
struct IsAllDefined<> {
    static constexpr bool value = true;
};

// 递归展开：判断第一个类型 + 剩余参数包
template <typename T, typename... Rest>
struct IsAllDefined<T, Rest...> {
    static constexpr bool value = JZMetaTypeId<remove_cvr_t<T>>::Defined && IsAllDefined<Rest...>::value;
};

// 判断tuple是否equal

// 辅助工具：判断类型是否为非常量引用（T&）
template <typename T>
struct is_const_lvalue_ref : std::false_type {};

template <typename T>
struct is_const_lvalue_ref<const T&> : std::true_type {};

// 核心：判断单个类型是否合法（T/const T/const T&）
template <typename T>
struct is_allowed_type {
    // 规则拆解：
    // 1. 如果是引用类型 → 必须是 const T&
    // 2. 如果不是引用类型 → 无论是否 const 都合法（T/const T）
    // 3. 自动排除 T&/T&&（因为引用类型但不是 const T&）
    static constexpr bool value =
        (!std::is_reference<T>::value) ||  // 非引用类型（T/const T）→ 合法
        is_const_lvalue_ref<T>::value;     // 是 const T& → 合法
};

// ------------------------ 检查元组所有元素都是允许的类型 ------------------------
template <typename Tuple>
struct tuple_all_allowed_types;

// 终止条件：空元组 → 符合要求
template <>
struct tuple_all_allowed_types<std::tuple<>> : std::true_type {};

// 递归检查：当前元素允许 && 剩余元素都允许
template <typename First, typename... Rest>
struct tuple_all_allowed_types<std::tuple<First, Rest...>> {
    static constexpr bool value =
        is_allowed_type<First>::value &&
        tuple_all_allowed_types<std::tuple<Rest...>>::value;
};

// 基础递归模板
template <typename FuncTuple, typename SignalTuple>
struct tuple_base_types_equal_up_to_signal_size;

template <>
struct tuple_base_types_equal_up_to_signal_size<std::tuple<>, std::tuple<>> {
    static constexpr bool value = true;
};

// 终止条件1：SignalTuple 为空（对比完成）
template <typename... FuncArgs>
struct tuple_base_types_equal_up_to_signal_size<std::tuple<FuncArgs...>, std::tuple<>> {
    static constexpr bool value = true;
};

// 终止条件2：SignalTuple 非空但 FuncTuple 为空（Func参数不足，对比失败）
template <typename... SignalArgs>
struct tuple_base_types_equal_up_to_signal_size<std::tuple<>, std::tuple<SignalArgs...>> {
    static constexpr bool value = false;
};

// 递归对比每个元素（C++14 兼容）
template <typename FuncFirst, typename... FuncRest, typename SignalFirst, typename... SignalRest>
struct tuple_base_types_equal_up_to_signal_size<
    std::tuple<FuncFirst, FuncRest...>,
    std::tuple<SignalFirst, SignalRest...>
> {
    // C++14：用 remove_cvref_t 替代 std::remove_cvref_t（手动组合）
    using FuncBase = remove_cvr_t<FuncFirst>;
    using SignalBase = remove_cvr_t<SignalFirst>;

    static constexpr bool value =
        // 当前元素基础类型一致
        std::is_same<FuncBase, SignalBase>::value&&
        // 递归对比下一个元素
        tuple_base_types_equal_up_to_signal_size<
        std::tuple<FuncRest...>,
        std::tuple<SignalRest...>
        >::value;
};

//check_signal_and_func_args
template <typename Func, typename... SignalArgs>
constexpr bool check_signal_and_func_args() {
    // 提取两个元组类型
    using FuncArgsTuple = typename jzsignal::function_traits<Func>::args_tuple;
    using SignalArgsTuple = std::tuple<SignalArgs...>;
    
    return std::tuple_size<SignalArgsTuple>::value >= std::tuple_size<FuncArgsTuple>::value //1. sig size > slot size
        // 2. 判断两个元组都不存在非常量引用
        && tuple_all_allowed_types<FuncArgsTuple>::value
        && tuple_all_allowed_types<SignalArgsTuple>::value
        // 3. 判断基础类型一致（仅比较到 SignalArgsTuple 的长度）
        && tuple_base_types_equal_up_to_signal_size<FuncArgsTuple, SignalArgsTuple>::value;
}

//slot call
template<typename R, typename C, typename... Args>
std::function<void(JZObject*, Args...)> make_slot_call(R(C::*func)(Args...))
{
    return [func](JZObject* obj, Args... args) {
        C* cobj = (C*)(obj);
        (cobj->*func)(std::forward<Args>(args)...); 
    };
}

template<typename... Args>
std::function<void(JZVariantList &list, Args...)> make_pack_call()
{
    return [](JZVariantList &list, Args... args) 
    {
        list.clear();

        using discard = int;
        (void)std::initializer_list<discard>{            
            (static_cast<void>(
                list.push_back(JZVariant::fromValue<remove_cvr_t<Args>>(std::forward<Args>(args)))
             ), 0)...
        };
    };
}

//调用slot
template<typename R, typename C, typename... Args, size_t... idx>
R slot_call_impl(C* obj, R(C::*func)(Args...), const JZVariantList& args, std::index_sequence<idx...>)
{
    return (obj->*func)(args[idx].value<remove_cvr_t<Args>>()...);
}

template<typename R, typename C, typename... Args>
std::function<void(JZObject*, const JZVariantList &list)> make_async_slot_call(R(C::*func)(Args...))
{
    return [func](JZObject *obj, const JZVariantList &list){
        C* cobj = (C*)(obj);
        slot_call_impl(cobj, func, list, std::make_index_sequence<sizeof...(Args)>());
    };
}

template<typename Func, typename... Args, size_t... idx>
void deal_slot_call_lambda_impl(Func func, const JZVariantList& args, std::index_sequence<idx...>)
{
    func(args[idx].value<remove_cvr_t<Args>>()...);
}

template<typename Func, typename Return, typename... Args>
std::function<void(const JZVariantList& args)> slot_call_lambda_impl(Func func, Return(*)(Args...))
{
    return [func](const JZVariantList& args) {
        deal_slot_call_lambda_impl<Func, Args...>(func, args, std::make_index_sequence<sizeof...(Args)>());
    };
}

template<typename Lambda>
std::function<void(const JZVariantList& list)> make_async_slot_lambda_call(Lambda func)
{
    return slot_call_lambda_impl(func, (typename strip_function_object<Lambda>::type*) nullptr);
}

} // namespace jzsignal


// 连接函数
template<typename... SignalArgs, typename Func>
std::enable_if_t<!jzsignal::IsAllDefined<SignalArgs...>::value, int>
connect(JZSignal<SignalArgs...>* signal, Func slot, JZConnectType type = AutoConnection)
{
    static_assert(jzsignal::check_signal_and_func_args<Func, SignalArgs...>(), "signal slot not match");
    JZ_ASSERT(type != QueuedConnection && type != BlockingQueuedConnection); //没定义meta,不能跨线程调用

    JZConnect<SignalArgs...> conn;
    conn.id = 0;
    conn.type = type;
    conn.hasRecv = false;
    conn.slot.lambdaFunc = slot;

    return signal->addConnect(conn);
}

template<typename... SignalArgs, typename Func>
std::enable_if_t<jzsignal::IsAllDefined<SignalArgs...>::value, int>
connect(JZSignal<SignalArgs...>* signal, Func slot, JZConnectType type = AutoConnection)
{
    static_assert(jzsignal::check_signal_and_func_args<Func, SignalArgs...>(), "signal slot not match");

    JZConnect<SignalArgs...> conn;
    conn.id = 0;
    conn.type = type;
    conn.hasRecv = false;
    conn.slot.lambdaFunc = slot;
    conn.slot.packFunc = jzsignal::make_pack_call<SignalArgs...>();
    conn.slot.asyncLambdaFunc = jzsignal::make_async_slot_lambda_call(slot);

    return signal->addConnect(conn);
}

template<typename... SignalArgs, typename Object, typename Func>
std::enable_if_t<!jzsignal::IsAllDefined<SignalArgs...>::value && std::is_member_pointer<Func>::value,int>
connect(JZSignal<SignalArgs...> *signal, Object *recv, Func slot, JZConnectType type = AutoConnection)
{
    static_assert(jzsignal::is_inherits_v<Object, Func>, "recv type not match");
    static_assert(jzsignal::check_signal_and_func_args<Func, SignalArgs...>(), "signal slot not match");
    JZ_ASSERT(type != QueuedConnection && type != BlockingQueuedConnection); //没定义meta,不能跨线程调用

    JZConnect<SignalArgs...> conn;
    conn.id = 0;
    conn.type = type;
    conn.hasRecv = true;
    conn.recv = recv->guardPointer();
    conn.slot.func = jzsignal::make_slot_call(slot);

    return signal->addConnect(conn);
}

template<typename... SignalArgs, typename Object, typename Func>
std::enable_if_t<jzsignal::IsAllDefined<SignalArgs...>::value && std::is_member_pointer<Func>::value,int>
connect(JZSignal<SignalArgs...>* signal, Object* recv, Func slot, JZConnectType type = AutoConnection)
{
    static_assert(jzsignal::is_inherits_v<Object, Func>, "recv type not match");
    static_assert(jzsignal::check_signal_and_func_args<Func, SignalArgs...>(), "signal slot not match");

    JZConnect<SignalArgs...> conn;
    conn.id = 0;
    conn.type = type;
    conn.hasRecv = true;
    conn.recv = recv->guardPointer();
    conn.slot.func = jzsignal::make_slot_call(slot);
    conn.slot.packFunc = jzsignal::make_pack_call<SignalArgs...>();
    conn.slot.asyncFunc = jzsignal::make_async_slot_call(slot);

    return signal->addConnect(conn);
}

template<typename... SignalArgs, typename Object, typename Func>
std::enable_if_t<!jzsignal::IsAllDefined<SignalArgs...>::value && !std::is_member_pointer<Func>::value, int>
connect(JZSignal<SignalArgs...>* signal, Object* recv, Func slot, JZConnectType type = AutoConnection)
{
    static_assert(jzsignal::check_signal_and_func_args<Func, SignalArgs...>(), "signal slot not match");
    JZ_ASSERT(type != QueuedConnection && type != BlockingQueuedConnection); //没定义meta,不能跨线程调用
    
    JZConnect<SignalArgs...> conn;
    conn.id = 0;
    conn.type = type;
    conn.hasRecv = true;
    conn.recv = recv->guardPointer();
    conn.slot.lambdaFunc = slot;

    return signal->addConnect(conn);
}

template<typename... SignalArgs, typename Object, typename Func>
std::enable_if_t<jzsignal::IsAllDefined<SignalArgs...>::value && !std::is_member_pointer<Func>::value, int>
connect(JZSignal<SignalArgs...>* signal, Object* recv, Func slot, JZConnectType type = AutoConnection)
{
    static_assert(jzsignal::check_signal_and_func_args<Func, SignalArgs...>(), "signal slot not match");

    JZConnect<SignalArgs...> conn;
    conn.id = 0;
    conn.type = type;
    conn.hasRecv = true;
    conn.recv = recv->guardPointer();
    conn.slot.lambdaFunc = slot;
    conn.slot.packFunc = jzsignal::make_pack_call<SignalArgs...>();
    conn.slot.asyncLambdaFunc = jzsignal::make_async_slot_lambda_call(slot);

    return signal->addConnect(conn);
}

template <typename... SignalArgs, typename Object>
void connectSignal(JZSignal<SignalArgs...> *signal, Object *object, JZSignal<SignalArgs...> *other_signal,
             JZConnectType type = AutoConnection)
{
    static_assert(std::is_base_of<JZObject, Object>::value,"connectSignal: Object must inherit from JZObject!");
    JZ_ASSERT(signal != other_signal);

    auto obj_ptr = object->guardPointer();
    auto func = [obj_ptr, other_signal](SignalArgs... args)
        {
            if (obj_ptr)
                other_signal->emit(std::forward<SignalArgs>(args)...);
        };

    connect(signal, func);
}

#endif // !JZ_SIGNAL_H_
