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
};
JZCoMainContext *g_mainCtx = nullptr;

class TaskMemPool {
public:
    // 构造函数：传入缓冲区起始地址和总字节数
    TaskMemPool(StackType_t* buffer, int size)
        : buffer_(reinterpret_cast<uint8_t*>(buffer)),
          total_size_(size),
          record_count_(0) {
        num_blocks_ = total_size_ / BLOCK_SIZE;
        if (num_blocks_ > MAX_BLOCKS) num_blocks_ = MAX_BLOCKS;
        // 初始化所有块为空闲（1表示空闲）
        for (int i = 0; i < num_blocks_; i++) {
            free_blocks_[i] = 1;
        }
        ESP_LOGI("TaskMemPool", "Init with %d blocks of %d bytes each", num_blocks_, BLOCK_SIZE);
    }

    // 分配内存，返回栈指针（失败返回 nullptr）
    // task_handle：如果非空，分配后立即记录关联；如果为空，只分配不记录，之后需调用 associate 绑定
    // size：所需字节数，至少 1，自动向上取整到块大小的倍数
    StackType_t* malloc(TaskHandle_t task_handle, size_t size) {
        if (size == 0) return nullptr;

        // 计算所需块数（向上取整）
        int blocks_needed = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        if (blocks_needed > num_blocks_) return nullptr;

        // 如果 task_handle 非空，检查是否已存在分配记录（防止重复分配）
        if (task_handle != nullptr) {
            for (int i = 0; i < record_count_; i++) {
                if (records_[i].handle == task_handle) {
                    ESP_LOGW("TaskMemPool", "Task already has allocation");
                    return nullptr;
                }
            }
        }

        // 扫描空闲块数组，查找第一个连续的空闲段
        int start = -1;
        int count = 0;
        for (int i = 0; i < num_blocks_; i++) {
            if (free_blocks_[i]) {
                if (start == -1) start = i;
                count++;
                if (count >= blocks_needed) {
                    // 找到足够的连续空闲块，标记为已分配（置0）
                    for (int j = start; j < start + blocks_needed; j++) {
                        free_blocks_[j] = 0;
                    }
                    // 记录分配信息（如果 task_handle 非空则立即记录，否则记录 handle = NULL）
                    if (record_count_ < MAX_TASKS) {
                        records_[record_count_].handle = task_handle;   // 可能为 NULL
                        records_[record_count_].start_block = start;
                        records_[record_count_].num_blocks = blocks_needed;
                        record_count_++;
                    } else {
                        // 记录数组已满，回滚分配
                        for (int j = start; j < start + blocks_needed; j++) {
                            free_blocks_[j] = 1;
                        }
                        ESP_LOGE("TaskMemPool", "Record array full");
                        return nullptr;
                    }
                    StackType_t* result = reinterpret_cast<StackType_t*>(buffer_ + start * BLOCK_SIZE);
                    ESP_LOGI("TaskMemPool", "Allocated %d blocks at %d for task %p",
                             blocks_needed, start, task_handle);
                    return result;
                }
            } else {
                start = -1;
                count = 0;
            }
        }
        ESP_LOGW("TaskMemPool", "No enough contiguous blocks");
        return nullptr;
    }

    // 关联任务句柄与已分配的栈（用于分配时 task_handle 为 NULL 的情况）
    // 通过栈地址查找之前分配的记录（handle 为 NULL），将其 handle 更新为传入值
    bool associate(TaskHandle_t handle, StackType_t* stack) {
        if (!handle || !stack) return false;

        uintptr_t addr = reinterpret_cast<uintptr_t>(stack);
        uintptr_t base = reinterpret_cast<uintptr_t>(buffer_);
        if (addr < base || addr >= base + num_blocks_ * BLOCK_SIZE) return false;

        int start_block = (addr - base) / BLOCK_SIZE;

        // 在记录中查找 handle 为 NULL 且起始块匹配的记录
        for (int i = 0; i < record_count_; i++) {
            if (records_[i].handle == nullptr && records_[i].start_block == start_block) {
                records_[i].handle = handle;
                ESP_LOGI("TaskMemPool", "Associated task %p with blocks starting at %d",
                         handle, start_block);
                return true;
            }
        }
        ESP_LOGW("TaskMemPool", "No unassociated allocation found for stack %p", stack);
        return false;
    }

    // 释放指定任务占用的内存块
    void free(TaskHandle_t task_handle) {
        if (!task_handle) return;

        // 查找记录
        int idx = -1;
        for (int i = 0; i < record_count_; i++) {
            if (records_[i].handle == task_handle) {
                idx = i;
                break;
            }
        }
        if (idx == -1) {
            ESP_LOGW("TaskMemPool", "Task %p not found", task_handle);
            return;
        }

        // 获取块信息
        int start = records_[idx].start_block;
        int num = records_[idx].num_blocks;

        // 将对应块标记为空闲（置1）
        for (int i = start; i < start + num; i++) {
            free_blocks_[i] = 1;
        }

        // 从记录数组中移除（用最后一个记录覆盖）
        records_[idx] = records_[--record_count_];
        ESP_LOGI("TaskMemPool", "Freed %d blocks starting at %d for task %p", num, start, task_handle);
    }

    // 调试：获取当前空闲块数量
    int getFreeBlockCount() const {
        int count = 0;
        for (int i = 0; i < num_blocks_; i++) {
            if (free_blocks_[i]) count++;
        }
        return count;
    }

private:
    uint8_t* buffer_;          // 缓冲区起始地址（对齐后）
    int total_size_;           // 总字节数
    int num_blocks_;           // 实际块数
    uint8_t free_blocks_[MAX_BLOCKS];  // 1=空闲, 0=已分配

    struct AllocRecord {
        TaskHandle_t handle;   // 可为 NULL（未关联）
        int start_block;
        int num_blocks;
    };
    AllocRecord records_[MAX_TASKS];
    int record_count_;
};

// ==================== 主上下文 ====================
JZCoMainContext::JZCoMainContext()
{
    ctx = xTaskGetCurrentTaskHandle();
    coMsgQueue = xQueueCreate(16, sizeof(CoMsg));
    pxTopOfStack = nullptr;
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
    int stack_size = (32 * 1024) / sizeof(StackType_t);
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
    StaticTask_t taskTask;
    static void CoEntryWrapper(void* param);
};

// ==================== 协程上下文 ====================
JZCoContext::JZCoContext(int stack_size)
    : task(nullptr), ctx_(nullptr)
{
    taskBuffer = xTaskGetStackStart(NULL) + stack_size/sizeof(StackType_t);
    ctx_ = xTaskCreateStatic(CoEntryWrapper, "co_task", 
        stack_size / sizeof(StackType_t), this, tskIDLE_PRIORITY + 1, taskBuffer, &taskTask);
    vTaskSuspend(ctx_);
}

JZCoContext::~JZCoContext()
{
    if (ctx_)
    {
        vTaskDelete(ctx_);
        ctx_ = nullptr;
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