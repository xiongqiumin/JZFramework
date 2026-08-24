#include "JZCoContext.h"
#include "JZCoCoroutine.h"
#include "JZDebug.h"

void JZCoContextEndTask(JZCoCoroutine* task)
{
    g_scheduler->endTask(task);
}

#if (JZ_OS == JZ_OS_WINDOWS) 

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

#elif (JZ_OS == JZ_OS_LINUX)

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
JZCoContext* createContext(JZCoCoroutine *co, int stack_size)
{    
    JZCoContext *coctx = new JZCoContext(stack_size);
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

#include "JZCore/private/JZFreeRTOS.h"

// 固定块大小
constexpr int BLOCK_SIZE = 512U;

class JZCOMemPool
{
public:
    void init(uint8_t* buffer, uint32_t bufSize)
    {
        m_poolBuf = buffer;
        m_totalSize = bufSize;
        m_totalBlocks = bufSize / BLOCK_SIZE;

        JZ_ASSERT(buffer != nullptr);
        JZ_ASSERT((bufSize % BLOCK_SIZE) == 0U);
        JZ_ASSERT(m_totalBlocks > 0U);

        m_blockState.assign(m_totalBlocks, 0U);
    }

    void* alloc(int allocSize)
    {
        JZ_ASSERT(allocSize > 0);
        JZ_ASSERT(m_totalBlocks > 0U);

        const uint32_t reqSize = static_cast<uint32_t>(allocSize);
        const uint32_t needBlocks = (reqSize + BLOCK_SIZE - 1U) / BLOCK_SIZE;

        JZ_ASSERT(needBlocks <= m_totalBlocks);
        JZ_ASSERT(getFreeBlocks() >= needBlocks);

        uint32_t consecutive = 0U;
        uint32_t startIdx = 0U;
        bool found = false;
        for (uint32_t i = 0; i < m_totalBlocks; ++i)
        {
            if (m_blockState[i] == 0U)
            {
                if (consecutive == 0U)
                    startIdx = i;
                consecutive++;
                if (consecutive >= needBlocks)
                {
                    found = true;
                    break;
                }
            }
            else
            {
                consecutive = 0U;
            }
        }
        JZ_ASSERT(found);

        m_blockState[startIdx] = needBlocks;
        for (uint32_t i = 1U; i < needBlocks; ++i)
        {
            m_blockState[startIdx + i] = 1U;
        }

        return m_poolBuf + startIdx * BLOCK_SIZE;
    }

    void free(void* ptr)
    {
        JZ_ASSERT(ptr != nullptr);
        JZ_ASSERT(m_poolBuf != nullptr);

        uint8_t* p = reinterpret_cast<uint8_t*>(ptr);
        const ptrdiff_t offset = p - m_poolBuf;
        const uint32_t offUint = static_cast<uint32_t>(offset);

        JZ_ASSERT(offset >= 0);
        JZ_ASSERT(offUint < m_totalSize);
        JZ_ASSERT((offUint % BLOCK_SIZE) == 0U);

        const uint32_t firstIdx = offUint / BLOCK_SIZE;
        const uint32_t blockCount = m_blockState[firstIdx];

        JZ_ASSERT(blockCount != 0U);
        JZ_ASSERT(blockCount <= (m_totalBlocks - firstIdx));

        for (uint32_t i = 1U; i < blockCount; ++i)
        {
            const uint32_t curIdx = firstIdx + i;
            JZ_ASSERT(m_blockState[curIdx] == 1U);
        }

        for (uint32_t i = 0U; i < blockCount; ++i)
        {
            m_blockState[firstIdx + i] = 0U;
        }
    }

    uint32_t getTotalBlocks() const
    {
        return m_totalBlocks;
    }

    uint32_t getFreeBlocks() const
    {
        uint32_t cnt = 0U;
        for (uint32_t val : m_blockState)
        {
            if (val == 0U)
                cnt++;
        }
        return cnt;
    }

    uint32_t getUsedBlocks() const
    {
        return m_totalBlocks - getFreeBlocks();
    }

    void reset()
    {
        if (m_totalBlocks > 0U)
            m_blockState.assign(m_totalBlocks, 0U);
    }

private:
    static constexpr uint32_t BLOCK_SIZE = 64U;

    uint8_t* m_poolBuf{ nullptr };
    uint32_t                m_totalSize{ 0U };
    uint32_t                m_totalBlocks{ 0U };
    std::vector<uint32_t>   m_blockState;
};

/*
切换原理,新建调度线程, 等待调度消息.收到调度消息后,修改main ctx 的 pxTopOfStack 切换到新的堆栈
*/
struct FreeRtosTaskControlBlock       /* The old naming convention is used to prevent breaking kernel aware debuggers. */
{
    StackType_t *pxTopOfStack;
};

struct CoMsg
{
    TaskHandle_t cur;
    TaskHandle_t next;
};

static void co_scheduler_task(void* arg);
class JZCoMainContext
{
public:
    JZCoMainContext();
    ~JZCoMainContext();

    QueueHandle_t coMsgQueue;
    TaskHandle_t ctx;
    StackType_t *pxTopOfStack;    
    JZCOMemPool memPool;
};
JZCoMainContext *g_mainCtx = nullptr;

// ==================== 主上下文 ====================
JZCoMainContext::JZCoMainContext()
{
    ctx = xTaskGetCurrentTaskHandle();
    coMsgQueue = xQueueCreate(16, sizeof(CoMsg));
    pxTopOfStack = nullptr;

    StackType_t *main_stack = pxTaskGetStackStart(NULL);
    char *buffer = (char *)main_stack;
    memPool.init((uint8_t * )buffer, 16 * 1024);
}

JZCoMainContext::~JZCoMainContext()
{
    CoMsg msg;
    msg.cur = nullptr;
    msg.next = nullptr;
    xQueueSend(g_mainCtx->coMsgQueue, &msg, portMAX_DELAY);

    vQueueDelete(coMsgQueue);
    coMsgQueue = nullptr;
}

JZCoMainContext *createMainContext()
{
    int stack_size = (1024) / sizeof(StackType_t);
    g_mainCtx = new JZCoMainContext();
    xTaskCreate(co_scheduler_task, "co_scheduler", stack_size, nullptr, configMAX_PRIORITIES - 1, NULL);
    return g_mainCtx;
}

void destoryMainContext(JZCoMainContext *ctx)
{
    delete ctx;
    g_mainCtx = nullptr;
}

CO_CONTEXT_HANDLE getMainContextHandle(JZCoMainContext* ctx)
{
    return ctx->ctx;
}

void co_scheduler_task(void *arg)
{
    BaseType_t ret;
    CoMsg msg;

    while (true)
    {
        ret = xQueueReceive(g_mainCtx->coMsgQueue, &msg, portMAX_DELAY);
        if (msg.cur == nullptr)
            break;

        if (ret == pdPASS)
        {
            auto cur_tcb = (FreeRtosTaskControlBlock*)msg.cur;
            auto next_tcb = (FreeRtosTaskControlBlock*)msg.next;
            if(msg.cur == g_mainCtx->ctx)
            {
                //main to co
                g_mainCtx->pxTopOfStack = cur_tcb->pxTopOfStack;
                cur_tcb->pxTopOfStack = next_tcb->pxTopOfStack;
            }
            else
            {
                //co to main
                cur_tcb->pxTopOfStack = next_tcb->pxTopOfStack; //保存当前堆栈到协程
                next_tcb->pxTopOfStack = g_mainCtx->pxTopOfStack; //恢复主线程堆栈
                g_mainCtx->pxTopOfStack = nullptr;
            }
        }
    }
    vTaskDelete(NULL);
}

// 协程上下文
class JZCoContext
{
public:
    JZCoContext(int stack_size);
    ~JZCoContext();

    JZCoCoroutine *task;
    TaskHandle_t ctx_;
    StackType_t *taskBuffer;
    StaticTask_t taskTask;  //tcb存放空间
    static void CoEntryWrapper(void* param);
};

// ==================== 协程上下文 ====================
JZCoContext::JZCoContext(int stack_size)
    : task(nullptr), ctx_(nullptr)
{
    //创建co时栈静态分配，保证空间合法.
    int cur_pri = uxTaskPriorityGet(NULL);
    taskBuffer = (StackType_t *)g_mainCtx->memPool.alloc(stack_size);
    if(!taskBuffer){
        JZ_ASSERT_X(0,"alloc co stack failed.");
    }
    ctx_ = xTaskCreateStatic(CoEntryWrapper, "co_task",
        stack_size / sizeof(StackType_t), this, cur_pri, taskBuffer, &taskTask);

    vTaskSuspend(ctx_);
}

JZCoContext::~JZCoContext()
{
    if (ctx_)
    {
        vTaskDelete(ctx_);
        ctx_ = nullptr;   
        g_mainCtx->memPool.free(taskBuffer);     
    }
}

void JZCoContext::CoEntryWrapper(void* param)
{
    JZCoContext *ctx = (JZCoContext *)param;    
    ctx->task->run();
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
    CoMsg msg;
    msg.cur = (TaskHandle_t)cur;
    msg.next = (TaskHandle_t)next;

    xQueueSend(g_mainCtx->coMsgQueue, &msg, portMAX_DELAY);
}

#elif (JZ_OS == JZ_OS_WASM)

#include <emscripten/fiber.h>

class JZCoMainContext
{
public:
    JZCoMainContext()
    {
        emscripten_fiber_init_from_current_context(&fiber, nullptr, 0);
    }
    ~JZCoMainContext() {}

    emscripten_fiber_t fiber;
};

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
    return &ctx->fiber;
}

class JZCoContext
{
public:
    JZCoContext(int stack_size)
    {
        stack.resize(stack_size);
    }
    ~JZCoContext()
    {
        task = nullptr;
    }

    JZCoCoroutine* task = nullptr;
    emscripten_fiber_t fiber;
    std::vector<char> stack;
};

static void emCoEntry(void* arg)
{
    auto* ctx = static_cast<JZCoContext*>(arg);
    ctx->task->run();
    JZCoContextEndTask(ctx->task);
}

JZCoContext* createContext(JZCoCoroutine *co, int stack_size)
{
    auto* c = new JZCoContext(stack_size);
    c->task = co;
    emscripten_fiber_create(&c->fiber, emCoEntry, c, c->stack.data(), stack_size);
    return c;
}

void destoryContext(JZCoContext *ctx)
{
    delete ctx;
}

CO_CONTEXT_HANDLE getCoContextHandle(JZCoContext* ctx)
{
    return &ctx->fiber;
}

void switchCoroutine(CO_CONTEXT_HANDLE cur, CO_CONTEXT_HANDLE next)
{
    emscripten_fiber_swap(static_cast<emscripten_fiber_t*>(cur),
                          static_cast<emscripten_fiber_t*>(next));
}

#else

#error "Unsupported OS for coroutine context switching"

#endif