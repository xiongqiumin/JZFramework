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
#include "JZTx.h"

#ifdef emit
#undef emit
#endif //emit

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
    struct function_traits JZFRAMEWORK_API : function_traits<decltype(&Lambda::operator())> {};

    /// Strip the class from a method type
    template <typename T> struct remove_class JZFRAMEWORK_API {};
    template <typename C, typename R, typename... A> struct remove_class<R(C::*)(A...)> { using type = R(A...); };
    template <typename C, typename R, typename... A> struct remove_class<R(C::*)(A...) const> { using type = R(A...); };

    template <typename F> struct strip_function_object JZFRAMEWORK_API {
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
    struct is_inherits_t JZFRAMEWORK_API {
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
    struct is_const_lvalue_ref JZFRAMEWORK_API : std::false_type {};

    template <typename T>
    struct is_const_lvalue_ref<const T&> : std::true_type {};

    // 核心：判断单个类型是否合法（T/const T/const T&）
    template <typename T>
    struct is_allowed_type JZFRAMEWORK_API {
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
    std::function<void(JZObject*, Args...)> make_slot_call(R(C::* func)(Args...))
    {
        return [func](JZObject* obj, Args... args) {
            C* cobj = (C*)(obj);
            (cobj->*func)(std::forward<Args>(args)...);
            };
    }

    //调用slot
    template<typename R, typename C, typename... Args, size_t... idx>
    R slot_call_impl(C* obj, R(C::* func)(Args...), const JZVariantList& args, std::index_sequence<idx...>)
    {
        return (obj->*func)(args[idx].valueRef<remove_cvr_t<Args>>()...);
    }

    template<typename R, typename C, typename... Args>
    std::function<void(JZObject*, const JZVariantList& list)> make_async_slot_call(R(C::* func)(Args...))
    {
        return [func](JZObject* obj, const JZVariantList& list) {
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

class JZFRAMEWORK_API JZBaseSlotCallback
{
public:
    virtual ~JZBaseSlotCallback(){}
    virtual void invoke(JZObject* obj, const JZVariantList& list) = 0;
};

// 1. 主模板：第一个参数用于 SFINAE，第二个参数包是信号参数类型
template<typename Enable, typename... Args>
class JZSlotCallback;

template<>
class JZFRAMEWORK_API JZSlotCallback<void> : public JZBaseSlotCallback
{
public:
    std::function<void(JZObject*)> memberFunc;
    std::function<void()> lambdaFunc;

    virtual void invoke(JZObject* obj, const JZVariantList& args) override
    {
        if (memberFunc)
            memberFunc(obj);
        else
            lambdaFunc();
    }
};

template<typename... Args>
class JZFRAMEWORK_API JZSlotCallback<std::enable_if_t<jzsignal::IsAllDefined<Args...>::value>, Args...> : public JZBaseSlotCallback
{
public:
    using MemberFunc = std::function<void(JZObject*, Args...)>;
    using LambdaFunc = std::function<void(Args...)>;

    // ---------- callLambda ----------
    // 版本 1：条件为真，正常解包并调用 Lambda
    virtual void invoke(JZObject* obj, const JZVariantList& args) override
    {
        invokeImpl(obj, args, std::index_sequence_for<Args...>{});
    }

    MemberFunc memberFunc;
    LambdaFunc lambdaFunc;

private:
    template<size_t... Is>
    void invokeImpl(JZObject* obj, const JZVariantList& args, std::index_sequence<Is...>)
    {
        if (memberFunc) {
            memberFunc(obj, args[Is].template valueRef<std::decay_t<Args>>()...);
        }
        else {
            lambdaFunc(args[Is].template valueRef<std::decay_t<Args>>()...);
        }
    }
};

template<typename... Args>
class JZFRAMEWORK_API JZSlotCallback<std::enable_if_t<!jzsignal::IsAllDefined<Args...>::value>, Args...> : public JZBaseSlotCallback
{
public:
    using MemberFunc = std::function<void(JZObject*, Args...)>;
    using LambdaFunc = std::function<void(Args...)>;

    // ---------- callLambda ----------
    // 版本 1：条件为真，正常解包并调用 Lambda
    virtual void invoke(JZObject*, const JZVariantList&) override
    {
        JZ_ASSERT(0);
    }

    MemberFunc memberFunc;
    LambdaFunc lambdaFunc;
};

class JZObject;
class JZThread;
//JZAsyncSignalEvent
class JZFRAMEWORK_API JZAsyncSignalEvent : public JZEvent
{
public:
    JZAsyncSignalEvent();
    ~JZAsyncSignalEvent();

    JZObject *sender;  //异步发送只检测接收,不检测发送者是否存在
    bool hasRecv;
    JZObjectPointer recv;
    JZBaseSlotCallback *callback;
    JZVariantList args;
};

//JZSLot
template<typename... Args>
class JZFRAMEWORK_API JZSlot
{
public:
    using MemberFunc = std::function<void(JZObject*, Args...)>;
    using LambdaFunc = std::function<void(Args...)>;

    // ============================================================
    // 1. 打包函数（packFunc）—— 已实现双版本
    // ============================================================
    template<typename T = void,
        typename std::enable_if_t<jzsignal::IsAllDefined<Args...>::value, T>* = nullptr>
    static void packFunc(JZVariantList& list, const Args&... args)
    {
        list.clear();
        list.reserve(sizeof...(Args));
        std::initializer_list<int>{
            (list.emplace_back(JZVariant::fromValue<std::decay_t<Args>>(args)), 0)...
        };
    }

    template<typename T = void,
        typename std::enable_if_t<!jzsignal::IsAllDefined<Args...>::value, T>* = nullptr>
    static void packFunc(JZVariantList&, const Args&...)
    {
        JZ_ASSERT(0 && "packFunc called with unregistered argument types");
    }

    bool isMember()
    {
        return memberFunc;
    }

    MemberFunc memberFunc;
    LambdaFunc lambdaFunc;
};

//JZConnect
template<typename... Args>
class JZFRAMEWORK_API JZConnect
{
public:
    int id;
    JZ::ConnectType type;
    bool hasRecv;          //是否拥有接受者
    JZObjectPointer recv;  //接受者
    JZSlot<Args...> slot;
};

class JZFRAMEWORK_API JZBaseSignal
{
public:
    JZBaseSignal();
    virtual ~JZBaseSignal();
    
    JZThread *objectThread(JZObject*sender);
    void pushSender(JZThread *thread, JZObject*sender);
    void popSender(JZThread *thread);

    JZObject *currentThread();
    bool isCurrentThread(JZObject *recv); 

    bool isBlockSingals(JZObject *object);
    JZObjectPointer guardPointer(JZObject *obj);
    void postEvent(JZAsyncSignalEvent* event);
    void postEvent(JZObject *recvier, JZAsyncSignalEvent *event);
    void postEventAndWait(JZObject *recvier, JZAsyncSignalEvent *event);
};

template<typename... Args>
class JZFRAMEWORK_API JZSignal : public JZBaseSignal
{
public:        
    JZSignal(JZObject *object)
    {
        m_object = object;
    }
    virtual ~JZSignal() = default;

    // 信号所属对象(构造时传入的 sender): 槽被调用时"以谁为 this"/"谁发的信号"。
    // 用途见 JZJSWidgetModule::callJsCallback(JS 回调里 self() 取它)。
    JZObject* object() const { return m_object; }

    int connectCount()
    {
        return m_connects ? (int)m_connects->size() : 0;
    }

    int addConnect(std::shared_ptr<JZConnect<Args...>> connect)
    {
        if (!m_connects) 
            m_connects.reset(new std::vector<std::shared_ptr<JZConnect<Args...>>>());
        
        int nextId = 0;
        for (auto& c : *m_connects)
        {
            if (c->id >= nextId)
                nextId = c->id + 1;
        }
        connect->id = nextId;
        m_connects->push_back(connect);
        return connect->id;
    }

    void clearConnect()
    {
        if (m_connects) m_connects->clear();
    }

    void removeById(int id)
    {
        if (!m_connects) return;
        for (int i = (int)m_connects->size() - 1; i >= 0; i--)
        {
            if ((*m_connects)[i]->id == id) {
                m_connects->erase(m_connects->begin() + i);
            }
        }
    }

    void removeRecvObject(JZObject *object)
    {
        if (!m_connects) return;
        for (int i = (int)m_connects->size() - 1; i >= 0; --i)
        {
            if ((*m_connects)[i].recv == object) {
                m_connects->erase(m_connects->begin() + i);
            }
        }
    }

    void emit(Args... args)
    {
        JZTX_FUNC_TRACE
        if (isBlockSingals(m_object) || !m_connects)
            return;
        
        auto connect_list = *m_connects;
        std::vector<int> deadIds;
        for (int i = 0; i < (int)connect_list.size(); i++)
        {
            auto& conn = *connect_list[i];
            
            int type = conn.type;
            if (conn.hasRecv)
            {
                //recv 可能在其他线程, 这里需要全局锁
                JZObjectPointerGuard guard(&conn.recv);
                JZObject *recvier = guard.object();
                if (!recvier)
                {
                    deadIds.push_back(conn.id);   // 收集已失效连接, 遍历结束后统一删除
                    continue;
                }

                bool is_same_thread = isCurrentThread(recvier);

                if ((type == JZ::AutoConnection && is_same_thread)
                    || type == JZ::DirectConnection)
                {
                    guard.unlock();   // 同步路径: 调用槽前解锁, 允许槽内重入/析构

                    JZThread* thread = objectThread(m_object);
                    JZObject* sender = m_object;

                    pushSender(thread, sender);
                    if (conn.slot.memberFunc)
                    {
                        conn.slot.memberFunc(recvier, args...);
                    }
                    else
                    {
                        conn.slot.lambdaFunc(args...);
                    }
                    popSender(thread);
                }
                else
                {
                    if (type == JZ::AutoConnection || type == JZ::QueuedConnection || type == JZ::BlockingQueuedConnection)
                    {
                        JZAsyncSignalEvent* event = new JZAsyncSignalEvent();
                        event->sender = m_object;
                        event->hasRecv = true;
                        event->recv = conn.recv;
                        conn.slot.packFunc(event->args, args...);

                        auto *callback = new JZSlotCallback<void, Args...>();
                        if (conn.slot.memberFunc)
                            callback->memberFunc = conn.slot.memberFunc;
                        else
                            callback->lambdaFunc = conn.slot.lambdaFunc;
                        event->callback = callback;

                        if (type == JZ::AutoConnection || type == JZ::QueuedConnection)
                            postEvent(recvier, event);
                        else
                            postEventAndWait(recvier, event);
                    }
                    else
                    {
                        JZ_ASSERT(0);
                    }
                }
            }
            else
            {
                if (type == JZ::AutoConnection || type == JZ::DirectConnection)
                {
                    conn.slot.lambdaFunc(args...);
                }
                else
                {
                    if (type == JZ::QueuedConnection)
                    {
                        JZAsyncSignalEvent* event = new JZAsyncSignalEvent();
                        event->sender = m_object;
                        conn.slot.packFunc(event->args, args...);

                        auto* callback = new JZSlotCallback<void, Args...>();
                        if (conn.slot.memberFunc)
                            callback->memberFunc = conn.slot.memberFunc;
                        else
                            callback->lambdaFunc = conn.slot.lambdaFunc;
                        event->callback = callback;

                        postEvent(event);
                    }
                    else
                    {
                        JZ_ASSERT(0);
                    }
                }
            }
        }

        // 清理本次 emit 中发现的失效连接
        for (int id : deadIds)
            removeById(id);
    }

protected:
    std::unique_ptr<std::vector<std::shared_ptr<JZConnect<Args...>>>> m_connects;  // 惰性: 无连接时为 nullptr
    JZObject *m_object;
};

typedef JZSignal<> JZSigVoid;
typedef JZSignal<bool> JZSigBool;
typedef JZSignal<int> JZSigInt;
typedef JZSignal<double> JZSigDouble;
typedef JZSignal<const std::string&> JZSigString;
typedef JZSignal<const JZByteArray&> JZSigByteArray;

// 连接函数
template<typename... SignalArgs, typename Func>
int connect(JZSignal<SignalArgs...>* signal, Func slot, JZ::ConnectType type = JZ::AutoConnection)
{
    static_assert(jzsignal::check_signal_and_func_args<Func, SignalArgs...>(), "signal slot not match");
    if(!jzsignal::IsAllDefined<SignalArgs...>::value) {
        // 类型未注册，检查连接类型是否合法
        JZ_ASSERT_X(type == JZ::AutoConnection || type == JZ::DirectConnection,
            "Queued/BlockingQueued connections require all signal argument types to be registered with JZVariant");
    }

    auto conn = std::make_shared<JZConnect<SignalArgs...>>();
    conn->id = 0;
    conn->type = type;
    conn->hasRecv = false;
    conn->slot.lambdaFunc = slot;

    return signal->addConnect(conn);
}

template<typename... SignalArgs, typename Object, typename Func>
std::enable_if_t<std::is_member_pointer<Func>::value,int>
connect(JZSignal<SignalArgs...> *signal, Object *recv, Func slot, JZ::ConnectType type = JZ::AutoConnection)
{
    static_assert(jzsignal::is_inherits_v<Object, Func>, "recv type not match");
    static_assert(jzsignal::check_signal_and_func_args<Func, SignalArgs...>(), "signal slot not match");
    if (!jzsignal::IsAllDefined<SignalArgs...>::value)
    {
        JZ_ASSERT_X(type == JZ::AutoConnection || type == JZ::DirectConnection,
            "Queued/BlockingQueued connections require all signal argument types to be registered with JZVariant");
    }

    auto conn = std::make_shared<JZConnect<SignalArgs...>>();
    conn->id = 0;
    conn->type = type;
    conn->hasRecv = true;
    conn->recv = recv->guardPointer();
    conn->slot.memberFunc = jzsignal::make_slot_call(slot);

    return signal->addConnect(conn);
}

template<typename... SignalArgs, typename Object, typename Func>
std::enable_if_t<!std::is_member_pointer<Func>::value, int>
connect(JZSignal<SignalArgs...>* signal, Object* recv, Func slot, JZ::ConnectType type = JZ::AutoConnection)
{
    static_assert(jzsignal::check_signal_and_func_args<Func, SignalArgs...>(), "signal slot not match");
    if(!jzsignal::IsAllDefined<SignalArgs...>::value) {
        // 类型未注册，检查连接类型是否合法
        JZ_ASSERT_X(type == JZ::AutoConnection || type == JZ::DirectConnection,
            "Queued/BlockingQueued connections require all signal argument types to be registered with JZVariant");
    }
    
    auto conn = std::make_shared<JZConnect<SignalArgs...>>();
    conn->id = 0;
    conn->type = type;
    conn->hasRecv = true;
    conn->recv = recv->guardPointer();
    conn->slot.lambdaFunc = slot;

    return signal->addConnect(conn);
}

template <typename... SignalArgs, typename Object>
void connectSignal(JZSignal<SignalArgs...> *signal, Object *object, JZSignal<SignalArgs...> *other_signal,
    JZ::ConnectType type = JZ::AutoConnection)
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
