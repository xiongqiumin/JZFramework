#include "JZTx.h"
#include "JZLog.h"
#include "JZPlatform.h"
#include "JZApplication.h"
#include "JZDebug.h"

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

#elif JZ_OS == JZ_OS_FREE_RTOS

#include "JZFreeRtos.h"

static int getCurrentStackUse()
{
    StackType_t dummy;
    StackType_t *sp = &dummy;
    StackType_t *stackLow = xTaskGetStackStart(NULL);
    UBaseType_t stackWordCnt = JZThread::currentThread()->config().stackSize / 4;
    StackType_t *stackHigh = stackLow + stackWordCnt;

    // 向下增长栈：已用栈 = 初始最高栈地址 - 当前SP
    return int((stackHigh - sp) * sizeof(StackType_t));
}

#else

static int getCurrentStackUse()
{
    return 0;
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
    JZTxTraceLog info = {};
    info.type = JZTxTraceLog::TraceOut;
    info.tid = m_tid;
    info.timestamp = JZPlatform::getTickCountMs();

    JZTxManager::instance()->addLog(info);
}

void JZTxTracer::recordIn(JZTxFixedStr fixed_str)
{
    m_tid = JZTxManager::instance()->currentTid();

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

bool JZTxManager::isInit() const
{
    return m_init;
}

JZTxFixedStr JZTxManager::addString(const std::string& input)
{
    JZMutexLocker lock(&m_mutex);
    if (!m_init)
        return JZTxFixedStr{"", 0};

    char* str = strdup(input.data());
    int size = (int)input.size();
    m_strList.push_back(str);
    return JZTxFixedStr{str, size};
}

void JZTxManager::collectCallStack(uint64_t tid, std::vector<JZTxTraceLog>& callStack)
{
    callStack.clear();
    
    // 从后往前收集该 tid 的所有未匹配 TraceIn
    for (auto it = m_logList.rbegin(); it != m_logList.rend(); ++it)
    {
        if (it->tid == tid && it->type == JZTxTraceLog::TraceIn)
        {
            callStack.push_back(*it);
        }
    }
}

void JZTxManager::addLog(const JZTxTraceLog& log)
{
    JZMutexLocker lock(&m_mutex);
    if (!m_init)
        return;

    if (log.type == JZTxTraceLog::TraceOut)
    {        
        // 倒序查找当前tid最近未匹配的TraceIn
        for (auto it = m_logList.rbegin(); it != m_logList.rend(); ++it)
        {
            if (it->tid == log.tid && it->type == JZTxTraceLog::TraceIn)
            {
                m_logList.erase(std::next(it).base());                
                break;
            }
        }
        // 无论是否匹配成功，TraceOut都不插入日志
        return;
    }    

    m_logList.push_back(log);
    if (log.type == JZTxTraceLog::TraceIn)
    {
        // TraceIn 入队前，先判断当前堆栈使用量
        int currentStackUsed = log.stackUsed;  // 当前堆栈使用量
        
        auto mapIt = m_maxStackLog.find(log.tid);
        if (mapIt == m_maxStackLog.end())
        {
            // 首次出现：直接记录最大值并收集调用栈
            MaxStackInfo info;
            info.maxStackSize = currentStackUsed;
            
            // 收集当前调用栈（不包含当前 TraceIn）
            std::vector<JZTxTraceLog> callStack;
            collectCallStack(log.tid, callStack);            
            info.stacks = callStack;   

            m_maxStackLog[log.tid] = info;
        }
        else if (currentStackUsed > mapIt->second.maxStackSize)
        {
            // 新的最大值：更新并收集调用栈
            mapIt->second.maxStackSize = currentStackUsed;
            
            std::vector<JZTxTraceLog> callStack;
            collectCallStack(log.tid, callStack);            
            mapIt->second.stacks = callStack;
        }      
    }       
}


void JZTxManager::clearLog(const JZTxTraceLog &log)
{
    JZMutexLocker lock(&m_mutex);    
    m_logList.clear();
}

uint64_t JZTxManager::currentTid()
{
    uint64_t tid = 0;
    #ifdef JZ_OS_NORMAL
        tid = std::hash<std::thread::id>()(JZThread::currentThreadId());
    #else
        tid = (uint64_t)JZThread::currentThreadId();
    #endif // JZ_OS == JZ_OS_NORMAL   

    return tid;
}

int JZTxManager::currentStackUse()
{
    return getCurrentStackUse();
}

void JZTxManager::printMaxBacktrace()
{
    JZMutexLocker lock(&m_mutex);
    const uint64_t cur_tid = currentTid();
    auto it = m_maxStackLog.find(cur_tid);
    if(it == m_maxStackLog.end())    
    {
        printf("Current thread(%" PRIu64 ") has no call stack.\n", cur_tid);
        return;
    }

    printStack(cur_tid, it->second.stacks);
}

void JZTxManager::printCurrentBacktrace()
{    
    JZMutexLocker lock(&m_mutex);

    const uint64_t cur_tid = currentTid();

    // 筛选当前线程所有未匹配的TraceIn，即为当前调用栈
    std::vector<JZTxTraceLog> threadCallStack;
    collectCallStack(cur_tid, threadCallStack);        
    printStack(cur_tid, threadCallStack);
}

void JZTxManager::printStack(uint64_t cur_tid,const std::vector<JZTxTraceLog>& threadCallStack)
{
    struct StackFrame {
        const char* func;
        int stackUsed; // 函数进入时的栈使用量
    };

    if (threadCallStack.empty())
    {
        printf("Current thread(%" PRIu64 ") has no call stack.\n", cur_tid);
        return;
    }

    // 组装栈帧
    std::vector<StackFrame> callStack;
    for (auto &log : threadCallStack)
    {
        callStack.push_back({ log.tag.str, log.stackUsed });
    }

    // 打印表头
    const int max_name_len = 42;
    printf("================ Current Thread Call Stack ================\n");
    printf("Thread ID: %" PRIu64 "\n\n", cur_tid);
    printf("  # | Function                   | Stack Depth (bytes) | Frame Size (bytes)\n");
    printf("----|----------------------------|---------------------|-------------------\n");

    // 遍历打印栈帧
    for (size_t i = 0; i < callStack.size(); ++i)
    {
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

        char displayName[max_name_len + 1] = { 0 };
        const char* funcName = frame.func ? frame.func : "(null)";
        strncpy(displayName, funcName, max_name_len);
        size_t nameLen = strlen(funcName);
        if (nameLen > max_name_len)
        {
            displayName[max_name_len - 3] = '.';
            displayName[max_name_len - 2] = '.';
            displayName[max_name_len - 1] = '.';
        }

        // 调用栈序号：从最外层函数往当前函数递增
        int seq = static_cast<int>(callStack.size() - 1 - i);
        printf("  %2d | %-40s | %19d | %17d\n",
               seq, displayName, frame.stackUsed, frameSize);
    }
    printf("===========================================================\n\n");
}