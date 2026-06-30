#include "JZTx.h"
#include "JZLog.h"
#include "JZPlatform.h"
#include "JZApplication.h"

#ifdef _WIN32
#include <Windows.h>
static int getCurrentStackUse()
{
    ULONG_PTR teb = __readgsqword(0x30);
    ULONG_PTR stackBase = *(ULONG_PTR*)(teb + 0x08);
    ULONG_PTR sp = (ULONG_PTR)_AddressOfReturnAddress();
    size_t used = stackBase - sp;
    return used;
}

#elif defined(__linux__)
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
static int getCurrentStackUse()
{
    pthread_attr_t attr;
    void* stackaddr;
    size_t stacksize;

    if (pthread_getattr_np(pthread_self(), &attr) != 0) {
        return 0;
    }
    if (pthread_attr_getstack(&attr, &stackaddr, &stacksize) != 0) {
        pthread_attr_destroy(&attr);
        return 0;
    }
    pthread_attr_destroy(&attr);

    volatile int dummy;
    uintptr_t sp = (uintptr_t)&dummy;
    uintptr_t stackBase = (uintptr_t)stackaddr;
    uintptr_t stackTop = stackBase + stacksize;

    if (sp < stackBase || sp > stackTop) {
        return 0;
    }
    return (int)(stackTop - sp);
}

#else JZ_OS == JZ_OS_FREE_RTOS

#include "JZFreeRtos.h"

static int getCurrentStackUse()
{
    // 【关键】必须用 volatile 修饰，防止编译器优化掉这个变量
    volatile uint32_t dummy;

    // 获取当前任务的控制块 (TCB)
    TCB_t* pxTCB = (TCB_t*)pxCurrentTCB;
    if (pxTCB == NULL) {
        return 0;
    }

    // 获取当前栈指针 (局部变量地址)
    uint8_t* sp = (uint8_t*)&dummy;
    // 获取栈的基地址 (栈的起始地址)
    uint8_t* stackBase = (uint8_t*)pxTCB->pxStack;

    size_t used = 0;

    // 判断栈增长方向 (大多数架构向下增长，如 ARM Cortex-M, x86)
#if (portSTACK_GROWTH < 0)  
    // 向下增长：栈从高地址向低地址增长，已用 = 基址 - 当前SP
    used = (stackBase > sp) ? (stackBase - sp) : 0;
#else  
    // 向上增长：栈从低地址向高地址增长 (极少见)
    used = (sp > stackBase) ? (sp - stackBase) : 0;
#endif

    return (int)used;
}

#endif // _WIN32


//JZTxTracer
JZTxTracer::JZTxTracer(const std::string &name)
{
    JZTxFixedStr str = JZTxManager::instance()->addString(name);
    recordIn(str);
}

JZTxTracer::JZTxTracer(JZTxFixedStr fixed_str)
{
    recordIn(fixed_str);
}

JZTxTracer::~JZTxTracer()
{
    int64_t end = JZPlatform::getTickCountMs();

    JZTxTraceLog info = {};
    info.type = JZTxTraceLog::TraceOut;
    info.tid = m_tid;
    info.timestamp = JZPlatform::getTickCountMs();

    JZTxManager::instance()->addLog(info);
}

void JZTxTracer::recordIn(JZTxFixedStr fixed_str)
{
#ifdef JZ_OS_NORMAL
    m_tid = std::hash<std::thread::id>()(JZThread::currentThreadId());
#else
    m_tid = (uint64_t)JZThread::currentThreadId();
#endif // JZ_OS == JZ_OS_NORMAL


    JZTxTraceLog info;
    info.type = JZTxTraceLog::TraceIn;
    info.tid = m_tid;
    info.timestamp = JZPlatform::getTickCountMs();
    info.tag = fixed_str;
    info.stackUsed = getCurrentStackUse();

    JZTxManager::instance()->addLog(info);
}

//JZTxConfig
JZTxConfig::JZTxConfig()
{
}

//JZTxManager
JZTxManager* JZTxManager::instance()
{
    static JZTxManager inst;
    return &inst;
}

JZTxManager::JZTxManager()
{
    m_init = false;
}

JZTxManager::~JZTxManager()
{
    for (auto str : m_strList)
        free(str);
    m_strList.clear();
}

void JZTxManager::init(JZTxConfig config)
{
    m_config = config;
    m_init = true;
}

JZTxFixedStr JZTxManager::addString(const std::string& input)
{
    JZMutexLocker lock(&m_mutex);
    if (!m_init)
        return JZTxFixedStr{nullptr, 0};

    char* str = strdup(input.data());
    int size = (int)input.size();
    m_strList.push_back(str);
    return JZTxFixedStr{str, size};
}

void JZTxManager::addLog(const JZTxTraceLog& log)
{
    JZMutexLocker lock(&m_mutex);
    if (!m_init)
        return;

    m_logList.push_back(log);
}

void JZTxManager::reportMaxStackTrace()
{
    struct StackFrame {
        const char* func;
        int stackUsed; // 进入时的栈使用量
    };

    JZMutexLocker lock(&m_mutex);
    if (m_logList.empty()) {
        printf("No logs recorded.\n");
        return;
    }

    // 1. 找到栈使用最大的 TraceIn 日志
    size_t maxIdx = 0;
    int maxStack = -1;
    bool found = false;
    for (size_t i = 0; i < m_logList.size(); ++i) {
        const JZTxTraceLog& log = m_logList[i];
        if (log.type == JZTxTraceLog::TraceIn && log.stackUsed > maxStack) {
            maxStack = log.stackUsed;
            maxIdx = i;
            found = true;
        }
    }

    if (!found) {
        printf("No TraceIn logs found.\n");
        return;
    }

    const JZTxTraceLog& peakLog = m_logList[maxIdx];
    printf("=== Peak Stack Usage ===\n");
    printf("Stack used: %d bytes\n", peakLog.stackUsed);
    printf("Timestamp : %" PRIi64 "\n", peakLog.timestamp);   // 假设时间戳不会超过 int 范围
    printf("Thread ID : %" PRIu64 "\n", peakLog.tid);         // 假设线程 ID 可转为 int
    printf("Tag       : %s\n", peakLog.tag.str ? peakLog.tag.str : "(null)");

    // 2. 重建调用堆栈
    std::vector<StackFrame> callStack;
    int pendingReturns = 0;

    callStack.push_back({ m_logList[maxIdx].tag.str, m_logList[maxIdx].stackUsed});
    for (int i = (int)maxIdx - 1; i >= 0; i--) {
        const JZTxTraceLog& log = m_logList[i];
        if (log.tid != peakLog.tid) 
            continue;

        if (log.type == JZTxTraceLog::TraceOut) {
            ++pendingReturns;
        }
        else if (log.type == JZTxTraceLog::TraceIn) {
            --pendingReturns;
            if(pendingReturns < 0)
            {
                callStack.push_back({ log.tag.str, log.stackUsed });
                pendingReturns = 0;
            }
        }
    }

    // 3. 打印调用堆栈（全部使用 %d）
    printf("\nCall Stack at Peak Stack Usage:\n");
    if (callStack.empty()) {
        printf("  (no call stack information)\n");
        return;
    }

    const int max_name_len = 42;
    printf("  # | Function                   | Stack Depth (bytes) | Frame Size (bytes)\n");
    printf("----|----------------------------|---------------------|-------------------\n");

    for (size_t i = 0; i < callStack.size(); ++i) {
        const auto& frame = callStack[i];
        int frameSize = 0;
        if (i < callStack.size() - 1)
        {
            frameSize = frame.stackUsed - callStack[i + 1].stackUsed;
        }
        else
        {
            frameSize = frame.stackUsed;
        }

        const char* funcName = frame.func ? frame.func : "(null)";
        char displayName[max_name_len + 1] = { 0 };
        strncpy(displayName, funcName, max_name_len);
        if (strlen(funcName) > max_name_len) {
            displayName[max_name_len - 1] = '.';
            displayName[max_name_len - 2] = '.';
            displayName[max_name_len - 3] = '.';
        }

        printf("  %2d | %-40s | %19d | %17d\n",
                (int)(callStack.size() - (i + 1)), displayName, frame.stackUsed, frameSize);
    }
}