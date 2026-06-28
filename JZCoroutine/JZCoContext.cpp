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
    JZCoContext()
    {
        task = nullptr;

        SIZE_T commit_size = 1024 * 1024;
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

JZCoContext* createContext(JZCoCoroutine *co)
{
    JZCoContext *coctx = new JZCoContext();    
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
    try
    {    
        ctx->task->run();
        JZCoContextEndTask(ctx->task);
    }
    catch (const JZCoCoroutineInterrupt&) 
    { 
        JZCoContextEndTask(ctx->task);
    }
}

#else

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
    JZCoContext()
    {
        ctx.isMainContext = false;
        stack.resize(1024 * 1024);
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

    try 
    {        
        next->task->run();
        JZCoContextEndTask(next->task);
    }
    catch (const JZCoCoroutineInterrupt&) 
    { 
        JZCoContextEndTask(next->task);
    }
}

CO_CONTEXT_HANDLE getMainContextHandle(JZCoMainContext* ctx)
{
    return &ctx->ctx;
}

CO_CONTEXT_HANDLE getCoContextHandle(JZCoContext* ctx)
{
    return &ctx->ctx;
}

#endif