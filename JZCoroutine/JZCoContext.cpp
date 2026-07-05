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

#include "JZFreeRTOS.h"

// 固定块大小
constexpr int BLOCK_SIZE = 512U;

class JZCOMemPool
{
public:
    /**
     * @brief 使用外部静态缓冲区初始化内存池
     * @param buffer 外部静态数组/缓冲区
     * @param bufSize 缓冲区总字节数
     */
    void init(const char* buffer, int bufSize)
    {
        m_poolBuf = reinterpret_cast<uint8_t*>(const_cast<char*>(buffer));
        m_totalSize = bufSize;
        m_totalBlocks = bufSize / BLOCK_SIZE;

        // 清空空闲块集合，初始化所有块为空闲
        m_freeBlocks.clear();
        for (int i = 0; i < m_totalBlocks; ++i)
        {
            m_freeBlocks.push_back(i);
        }
    }

    /**
     * @brief 内存分配，最小分配512字节
     * @param allocSize 申请字节大小
     * @return 分配内存指针，nullptr分配失败
     */
    void* alloc(int allocSize)
    {
        if (m_freeBlocks.empty())
        {
            printf("MemPool alloc failed: no free blocks\n");
            return nullptr;
        }

        // 最少分配1块，向上取整计算需要块数
        int needBlocks = 1;
        if (allocSize > BLOCK_SIZE)
        {
            needBlocks = (allocSize + BLOCK_SIZE - 1) / BLOCK_SIZE;
        }

        // 空闲块不足
        if (needBlocks > m_freeBlocks.size())
        {
            printf("MemPool alloc failed: need %zu blocks, free %zu\n", needBlocks, m_freeBlocks.size());
            return nullptr;
        }

        // 取出末尾N个空闲块（vector尾部弹出效率最高）
        int firstBlockIdx = m_freeBlocks[m_freeBlocks.size() - needBlocks];
        for (int i = 0; i < needBlocks; ++i)
        {
            m_freeBlocks.pop_back();
        }

        // 返回对应块起始地址
        return m_poolBuf + firstBlockIdx * BLOCK_SIZE;
    }

    /**
     * @brief 释放内存块
     * @param ptr alloc返回的内存指针
     */
    void free(void* ptr)
    {
        if (ptr == nullptr || m_poolBuf == nullptr)
            return;

        // 计算块下标
        ptrdiff_t offset = reinterpret_cast<uint8_t*>(ptr) - m_poolBuf;
        if (offset < 0 || static_cast<int>(offset) % BLOCK_SIZE != 0)
        {
            printf("MemPool free failed: invalid pointer\n");
            return;
        }

        int blockIdx = static_cast<int>(offset) / BLOCK_SIZE;

        // 查找该块是否已经空闲，防止重复释放
        for (int idx : m_freeBlocks)
        {
            if (idx == blockIdx)
            {
                printf("MemPool free warning: double free\n");
                return;
            }
        }

        // 归还到空闲块vector
        m_freeBlocks.push_back(blockIdx);
    }

    // 状态查询接口
    int getTotalBlocks() const  { return m_totalBlocks; }
    int getFreeBlocks() const   { return m_freeBlocks.size(); }
    int getUsedBlocks() const   { return m_totalBlocks - m_freeBlocks.size(); }

private:
    uint8_t*               m_poolBuf{nullptr};
    int                 m_totalSize{0};
    int                 m_totalBlocks{0};
    // vector存储所有空闲块的下标，替代链表
    std::vector<int>    m_freeBlocks;
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
    memPool.init(buffer, 16 * 1024);
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

#else

#error "Unsupported OS for coroutine context switching"

#endif