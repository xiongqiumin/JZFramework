#ifndef JZ_TX_H_
#define JZ_TX_H_

#include <inttypes.h>
#include <string>
#include <vector>
#include <map>
#include "JZGlobal.h"
#include "JZMutex.h"

struct JZTxFixedStr
{
    const char* str;
    int len;
};

struct JZTxTraceLog
{
    enum LogType{
        TraceNone,
        TraceIn,
        TraceOut,
    };

    LogType type;
    int64_t timestamp;
    uint64_t tid;
    JZTxFixedStr tag;
    int stackUsed;
};

class JZTxTracer
{
public:
    JZTxTracer(const std::string &name);
    JZTxTracer(JZTxFixedStr fixed_str);
    ~JZTxTracer();

protected:
    void recordIn(JZTxFixedStr fixed_str);

    uint64_t m_tid;
};

#if 1
#define JZTX_TRACE(name) JZTxTracer __func_scoped_range##__LINE__(name);
#define JZTX_FUNC_TRACE  JZTxTracer __func_scoped_range##__LINE__(JZTxFixedStr{__FUNCTION__, sizeof(__FUNCTION__) - 1});
#else
#define JZTX_TRACE(name)
#define JZTX_FUNC_TRACE
#endif

class JZTxConfig
{
public:
    JZTxConfig();
};

class JZTxClient;
class JZTxManager
{
public:
    static JZTxManager* instance();

    JZTxManager();
    ~JZTxManager();
    
    void init(JZTxConfig config);
    bool isInit() const;
    
    uint64_t currentTid();
    int currentStackUse();
    void printCurrentBacktrace();
    void printMaxBacktrace();

    JZTxFixedStr addString(const std::string& str);
    void addLog(const JZTxTraceLog &log);
    void clearLog(const JZTxTraceLog &log);

protected:
    friend JZTxClient;

    struct MaxStackInfo
    {
        int maxStackSize = 0;
        std::vector<JZTxTraceLog> stacks;
    };

    void collectCallStack(uint64_t tid, std::vector<JZTxTraceLog>& callStack);
    void printStack(uint64_t tid, const std::vector<JZTxTraceLog>& callStack);

    JZMutex m_mutex;
    std::vector<char*> m_strList;
    std::vector<JZTxTraceLog> m_logList;    
    std::map<uint64_t, MaxStackInfo> m_maxStackLog;
    
    bool m_init;
    JZTxConfig m_config;
};

#endif // !JZ_TX_H_
