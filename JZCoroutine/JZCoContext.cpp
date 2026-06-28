#include "JZCoContext.h"
#include "JZCoCoroutine.h"
#include "JZDebug.h"

void JZCoContextEndTask(JZCoCoroutine* task)
{
    g_scheduler->endTask(task);
}

#ifdef _WIN32 

#include <Windows.h>

//JZMainContext
class JZCoMainContext
{
public:
    JZCoMainContext()
    {
        ctx = ConvertThreadToFiber(NULL);
    }

    ~JZCoMainContext()
    {
        ConvertFiberToThread();
    }

    void *ctx;
};

JZCoMainContext *createMainContext()
{
    JZCoMainContext *ctx = new JZCoMainContext();
    return ctx;
}

void destoryMainContext(JZCoMainContext *ctx)
{
    delete ctx;
}

CO_CONTEXT_HANDLE getMainContextHandle(JZCoMainContext* ctx)
{
    return ctx->ctx;
}

//JZCoContext
VOID CALLBACK FiberFunction(LPVOID lpFiberParameter);
class JZCoContext
{
public:
    JZCoContext(int stack_size)
    {
        task = nullptr;

        SIZE_T commit_size = stack_size;
        ctx_ = CreateFiberEx(commit_size, commit_size, FIBER_FLAG_FLOAT_SWITCH, (LPFIBER_START_ROUTINE)FiberFunction, (LPVOID)this);
        JZ_ASSERT(ctx_);
    }
    ~JZCoContext()
    {
        DeleteFiber(ctx_);
    }

    JZCoCoroutine *task;
    void *ctx_;
};

JZCoContext* createContext(JZCoCoroutine *co,int stack_size)
{
    JZCoContext *coctx = new JZCoContext(stack_size);    
    coctx->task = co;
    return coctx;
}

void destoryContext(JZCoContext *ctx)
{
    delete ctx;
}

void switchCoroutine(CO_CONTEXT_HANDLE cur, CO_CONTEXT_HANDLE next)
{
    SwitchToFiber(next);
}

CO_CONTEXT_HANDLE getCoContextHandle(JZCoContext* ctx)
{
    return ctx->ctx_;
}

// 纤程执行的函数
VOID CALLBACK FiberFunction(LPVOID lpFiberParameter)
{
    JZCoContext *ctx = (JZCoContext *)lpFiberParameter;    
    ctx->task->run();
    JZCoContextEndTask(ctx->task);    
}

#elif defined(__linux__)

#include "boost_context/fcontext.hpp"

namespace boost_context = boost::context::detail;

struct ContextInfo 
{
    ContextInfo()
    {
        isMainContext = false;
        ctx = nullptr;
        task = nullptr;
    }

    bool isMainContext;
    JZCoCoroutine *task;
    boost_context::fcontext_t ctx;
};

struct ContextSwitch
{
    ContextInfo *from;
    ContextInfo *to;
};

class JZCoMainContext
{
public:    
    JZCoMainContext()
    {
        ctx.isMainContext = true;
    }

    ContextInfo ctx;
};

class JZCoContext
{
public:    
    JZCoContext(int stack_size)
    {
        ctx.isMainContext = false;
        stack.resize(stack_size);
    }

    ContextInfo ctx;
    std::vector<char> stack;
};

//JZCoMainContext
JZCoMainContext *createMainContext()
{
    JZCoMainContext *mainctx = new JZCoMainContext();
    return mainctx;
}

void destoryMainContext(JZCoMainContext *ctx)
{
    delete ctx;
}

//JZCoCoroutine
void FiberFunction(boost_context::transfer_t t);
JZCoContext* createContext(JZCoCoroutine *co)
{    
    JZCoContext *coctx = new JZCoContext();
    coctx->ctx.task = co;
    
    // 为协程创建 fiber_context，使用固定大小的堆栈
    coctx->ctx.ctx = boost_context::make_fcontext(
        coctx->stack.data() + coctx->stack.size(),
        coctx->stack.size(),
        FiberFunction
    );
    
    return coctx;
}

void destoryContext(JZCoContext *ctx)
{
    delete ctx;
}

// 上下文切换函数
void switchCoroutine(CO_CONTEXT_HANDLE cur, CO_CONTEXT_HANDLE next)
{
    ContextSwitch ctx_switch;
    ContextInfo *cur_ctx = static_cast<ContextInfo*>(cur);
    ContextInfo *next_ctx = static_cast<ContextInfo*>(next);
    ctx_switch.from = cur_ctx;
    ctx_switch.to = next_ctx;
    
    auto jump_ret = boost_context::jump_fcontext(next_ctx->ctx, &ctx_switch);
    ContextSwitch *ret_ctx = (ContextSwitch*)jump_ret.data;
    ret_ctx->from->ctx = jump_ret.fctx;
}

/*
transfer_t.fctx 就是 from 的上下文, 通过
jump_fcontext(transfer_t.fctx)  可以切换原先的堆栈
*/
void FiberFunction(boost_context::transfer_t t)
{
    ContextSwitch* coctx = (ContextSwitch*)t.data;
    coctx->from->ctx = t.fctx;

    ContextInfo *next = coctx->to;
    next->ctx = nullptr;
    
    next->task->run();
    JZCoContextEndTask(next->task);
}

CO_CONTEXT_HANDLE getMainContextHandle(JZCoMainContext* ctx)
{
    return &ctx->ctx;
}

CO_CONTEXT_HANDLE getCoContextHandle(JZCoContext* ctx)
{
    return &ctx->ctx;
}

#elif (JZ_OS == JZ_OS_FREE_RTOS)

#include <freertos/FreeRTOS.h>

// 主上下文（当前FreeRTOS任务作为主线程主纤程）
class JZCoMainContext
{
public:
    JZCoMainContext();
    ~JZCoMainContext();

    TaskHandle_t ctx;
};

// ==================== 主上下文 ====================
JZCoMainContext::JZCoMainContext()
{
    // 等价 ConvertThreadToFiber：把当前任务作为主纤程
    ctx = xTaskGetCurrentTaskHandle();
}

JZCoMainContext::~JZCoMainContext()
{
    // 等价 ConvertFiberToThread：无需额外操作，任务生命周期由外部管理
}

JZCoMainContext *createMainContext()
{
    return new JZCoMainContext();
}

void destoryMainContext(JZCoMainContext *ctx)
{
    delete ctx;
}

CO_CONTEXT_HANDLE getMainContextHandle(JZCoMainContext* ctx)
{
    return ctx->ctx;
}

// 协程上下文
class JZCoContext
{
public:
    JZCoContext(int stack_size);
    ~JZCoContext();

    JZCoCoroutine *task;
    TaskHandle_t ctx_;
    static void CoEntryWrapper(void* param);
};

// ==================== 协程上下文 ====================
JZCoContext::JZCoContext(int stack_size)
    : task(nullptr), ctx_(nullptr)
{
    jzDebug() << "JZCoContext" << this;

    // 创建任务，创建后立刻挂起，等价 CreateFiberEx
    BaseType_t ret = xTaskCreate(
        JZCoContext::CoEntryWrapper,
        "jz_co",
        stack_size/4,
        this,
        uxTaskPriorityGet(NULL), // 和主任务同优先级
        &ctx_
    );
    JZ_ASSERT(ret == pdPASS);
    // 创建完成立即挂起，等待手动切换调度
    vTaskSuspend(ctx_);
}

JZCoContext::~JZCoContext()
{
    if (ctx_)
    {
        vTaskDelete(ctx_);
    }
}

void JZCoContext::CoEntryWrapper(void* param)
{
    JZCoContext* ctx = static_cast<JZCoContext*>(param);
    jzDebug() << "CoEntryWrapper" << ctx;
    jzDebug() << "ctx->task" << ctx->task;
    ctx->task->run();
    // 协程执行完毕，回收资源
    JZCoContextEndTask(ctx->task);    
}

JZCoContext* createContext(JZCoCoroutine *co, int stack_size)
{
    JZCoContext *coctx = new JZCoContext(stack_size);
    coctx->task = co;
    return coctx;
}

void destoryContext(JZCoContext *ctx)
{
    delete ctx;
}

CO_CONTEXT_HANDLE getCoContextHandle(JZCoContext* ctx)
{
    return ctx->ctx_;
}

// ==================== 协程切换（核心） ====================
void switchCoroutine(CO_CONTEXT_HANDLE cur, CO_CONTEXT_HANDLE next)
{
    TaskHandle_t hCur = static_cast<TaskHandle_t>(cur);
    TaskHandle_t hNext = static_cast<TaskHandle_t>(next);

    // 挂起当前运行的协程
    vTaskSuspend(hCur);
    // 唤醒目标协程
    vTaskResume(hNext);
    // 触发FreeRTOS原生PendSV上下文切换，复用官方寄存器保存恢复
    taskYIELD();
}

#else

#error "un support"

#endif