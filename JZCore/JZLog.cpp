#include <assert.h>
#include <stdarg.h>
#include "JZLog.h"
#include "JZDateTime.h"
#include "JZApplication.h"
#include "JZTimer.h"

#if defined(_WIN32)
#include <windows.h>   // GetConsoleMode / GetConsoleOutputCP: 控制台输出编码判定, 见 toConsoleBytes
#endif

#ifdef __ANDROID__
#include <android/log.h>

int toAndroidLevel(int level)
{
    switch (level) 
    {
        case LOG_DEBUG:
            return ANDROID_LOG_DEBUG;
        case LOG_INFO:
            return ANDROID_LOG_INFO;
        case LOG_WARNING:
            return ANDROID_LOG_WARN;   
        case LOG_ERROR:
            return ANDROID_LOG_ERROR;  
        case LOG_FATAL:
            return ANDROID_LOG_FATAL;   
        default:
            return ANDROID_LOG_VERBOSE;
    }
}
#endif

JZLogModuleConfig::JZLogModuleConfig()
{
    type = None;
}

//JZLogItem
std::shared_ptr<JZLogItem> JZLogItem::makeLog(int module, int level, std::string message)
{
    JZLogItemPtr log_ptr = JZLogItemPtr(new JZLogItem());
    log_ptr->module = module;
    log_ptr->level = level;
    log_ptr->time = JZDateTime::currentMSecsSinceEpoch();
    log_ptr->message = message;
    return log_ptr;
}

//JZLogEvent
JZLogEvent::JZLogEvent()
    :JZEvent(JZEvent_log)
{
}


//JZLogManager
static bool g_logEnabled = false;
JZLogManager *JZLogManager::instance()
{
    static JZLogManager inst;
    return &inst;
}

JZLogManager::JZLogManager()
{
    g_logEnabled = true;
    
    JZLogModuleConfig config;
    config.name = "General";
    config.type = JZLogModuleConfig::Console;
    addModule(LogModule_General, config);
}

JZLogManager::~JZLogManager()
{
    // ⚠ 单例是函数内 static, 析构在 main 之后 —— 那时 JZApplication 通常已经没了
    // (jzApp == nullptr), 定时器所在的事件循环也没了, 不能再碰它。漏一个定时器
    // 在退出阶段无所谓。
    if (jzApp && m_flushTimer)
    {
        delete m_flushTimer;
        m_flushTimer = nullptr;
    }

    flushFile();
          
    jzDeleteAll(m_modules);
    m_modules.clear();

    g_logEnabled = false;
}

JZLogConfig JZLogManager::config()
{
    return m_config;
}

void JZLogManager::setConfig(JZLogConfig config)
{
    m_config = config;
    updateFlushTimer();
}

void JZLogManager::addModule(int module, JZLogModuleConfig config)
{    
    JZLogModule *mod = new JZLogModule();
    mod->config = config;
    m_modules[module] = mod;
}

std::string JZLogManager::moduleName(int module)
{
    if (!map_contains(m_modules, module))
        return std::string();

    return m_modules[module]->config.name;
}

JZLogModuleConfig JZLogManager::moduleConfig(int id)
{
    return m_modules[id]->config;
}

void JZLogManager::setModuleConfig(int id, JZLogModuleConfig config)
{
    assert(map_contains(m_modules,id));
    m_modules[id]->config = config;

    if (config.type & JZLogModuleConfig::File)
    {
        std::string file_path = m_config.logPath + "/" + config.name + ".log";        
        m_modules[id]->file.open(file_path, "wt");
    }
    else
    {
        m_modules[id]->file.close();
    }

    updateFlushTimer();
}

void JZLogManager::addObserver(int module, JZObject *recv)
{
    if (!map_contains(m_modules, module))
        return;

    m_modules[module]->recvObjects.push_back(recv);
}

void JZLogManager::removeObserver(int module, JZObject *recv)
{
    if (!map_contains(m_modules,module))
        return;

    vec_removeAll(m_modules[module]->recvObjects, recv);
}

void JZLogManager::updateFlushTimer()
{
    if (!jzApp)
        return;

    bool hasFileModule = false;
    auto it = m_modules.begin();
    while (it != m_modules.end())
    {
        if (it->second->config.type & JZLogModuleConfig::File)
            hasFileModule = true;
        it++;
    }

    if (!hasFileModule || m_config.flushIntervalMs <= 0)
    {
        if (m_flushTimer)
            m_flushTimer->stop();
        return;
    }

    if (!m_flushTimer)
    {
        m_flushTimer = new JZTimer();
        connect(&m_flushTimer->sigTimeout, [this]() { flushFile(); });
    }

    // 间隔可能刚被改过, 直接重启一次(定时器空闲时 stop 是安全的)
    m_flushTimer->stop();
    m_flushTimer->start(m_config.flushIntervalMs);
}

void JZLogManager::flushFile()
{
    // log() 可能正在别的线程往 logCache 里塞, 这里得锁着搬
    JZMutexLocker locker(&m_lock);

    auto it = m_modules.begin();
    while (it != m_modules.end())
    {
        auto &logs = it->second->logCache;
        // 文件没开(File 被关掉了 / 路径打不开): 写不进去就丢掉, 别攒着涨内存
        if (it->second->file.isOpen())
        {
            for (int line = 0; line < (int)logs.size(); line++)
            {
                auto log = logs[line].get();

                JZDateTime time = JZDateTime::fromMSecsSinceEpoch(log->time);
                std::string log_save = time.toString("[yyyy-MM-dd HH:mm:ss zzz] ");
                log_save += log->message + "\n";

                it->second->file.write(log_save.c_str(), (int)log_save.size());
            }
            // 光 write 不够: 那只是进了 stdio 的缓冲区, 得 fflush 才真的落盘
            it->second->file.flush();
        }
        logs.clear();

        it++;
    }
}

const char* JZLogManager::getLevelPrefix(int level)
{
    switch (level) {
        case LOG_WARNING: return "[WARNING]";  // 黄色
        case LOG_ERROR:   return "[ERROR]";  // 红色
        case LOG_FATAL:   return "[FATAL]"; // 红底白字
        default:          return nullptr;
    }
}

// ======================== 控制台输出的字节 ========================
// printf 到 stdout 的字节按 **stdout 的实际状态**决定 —— 不能用 CP_ACP:
//   · 真控制台(GetConsoleMode 成功)且控制台 CP 不是 UTF-8(cmd 默认 936)
//     ⇒ 转成那个 CP 再打(不转就是乱码)
//   · 真控制台 + CP = 65001(chcp 65001 / PowerShell 7 / Windows Terminal)
//     ⇒ 原样 UTF-8(再按 CP_ACP 转反而把中文写坏)
//   · stdout 被重定向/是管道(python 抓日志、`>`、mintty/Git-Bash) ⇒ 原样 UTF-8
//     (接收方按 UTF-8 解; CP_ACP 是系统 ANSI 代码页, 跟控制台/管道状态无关, chcp 也改不动它
//      —— 老代码在这里无脑转 CP_ACP, 于是"管道/UTF-8 终端里的中文变乱码")
//   · 没有控制台(GUI 程序, 未 AttachConsole) ⇒ 原样(写了也没人看)
// 状态**只探测一次**(第一次日志调用时): 句柄/控制台 CP 在一次进程生命周期内基本不变, 而
// log() 是高频路径。⚠ 运行中途 chcp 换编码不会跟着变(要跟就得去掉 static)。
//
// 注: 与 str_utf8_to_local(CP_ACP) 不是一回事 —— 那个是给 ANSI **文件** API(JZFile/JZDir)
//     用的, 那边必须转, 别混。
#if defined(_WIN32)
// UTF-8 -> 指定代码页(cp 用控制台自己的 CP, 而不是 CP_ACP)
static std::string utf8ToCodepage(const std::string &utf8, UINT cp)
{
    if (utf8.empty())
        return utf8;

    int wcsLen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wcsLen <= 1)
        return utf8;

    std::wstring wstr(wcsLen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wstr[0], wcsLen);

    int len = WideCharToMultiByte(cp, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return utf8;

    std::string out(len - 1, '\0');
    WideCharToMultiByte(cp, 0, wstr.c_str(), -1, &out[0], len, nullptr, nullptr);
    return out;
}
#endif

static std::string toConsoleBytes(const std::string &utf8)
{
#if defined(_WIN32)
    struct ConsoleState
    {
        bool isConsole;
        UINT cp;
    };

    static const ConsoleState st = []() -> ConsoleState
    {
        ConsoleState s;
        s.isConsole = false;
        s.cp = CP_UTF8;

        HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (h != nullptr && h != INVALID_HANDLE_VALUE && ::GetConsoleMode(h, &mode))
        {
            s.isConsole = true;
            s.cp = ::GetConsoleOutputCP();
        }
        return s;
    }();

    if (!st.isConsole || st.cp == CP_UTF8 || st.cp == 0)
        return utf8;
    return utf8ToCodepage(utf8, st.cp);
#else
    // 非 Windows: 本地编码就是 UTF-8(JZStd.cpp 里 str_utf8_to_local 也是透传), 原样输出
    return utf8;
#endif
}

void JZLogManager::log(int module_id, int level, const std::string &message)
{
    JZMutexLocker locker(&m_lock);
    if (!g_logEnabled || !map_contains(m_modules,module_id))
        return;

    auto module = m_modules[module_id];    
    auto cur_time = JZDateTime::currentDateTime();

    //JZLogItemPtr
    JZLogItemPtr log_ptr = JZLogItemPtr(new JZLogItem());
    log_ptr->module = module_id;
    log_ptr->level = level;
    log_ptr->time = cur_time.toMSecsSinceEpoch();
    log_ptr->message = message;
    if (module->config.type & JZLogModuleConfig::File)
    {
        module->logCache.push_back(log_ptr);
    }

    if (module->config.type & JZLogModuleConfig::Console)
    {        
#ifdef __ANDROID__
        int android_level = toAndroidLevel(log_ptr->level);
        __android_log_print(android_level, "JZLOG", "%s", message.c_str());      
#else
    if (m_config.shouldPrintTime)
    {
        std::string time_str = cur_time.toString("[HH:mm:ss.zzz]");
        printf("%s ", time_str.c_str());
    }

    auto pre = getLevelPrefix(level);
    if(pre)
        printf("%s ",pre);

    printf("%s\n", toConsoleBytes(message).c_str());

#endif
    }

    // 退出临界区后再派发事件, 避免持锁调用 postEvent 引发锁顺序反转/死锁
    std::vector<JZObject *> recvObjects = module->recvObjects;
    std::string fatalMsg;
    bool isFatal = (level == LOG_FATAL);
    if (isFatal)
        fatalMsg = message;

    locker.unlock();

    for (int i = (int)recvObjects.size() - 1; i >= 0; i--)
    {
        JZLogEvent *event = new JZLogEvent();
        event->log = log_ptr;
        jzApp->postEvent(recvObjects[i], event);
    }

    if (isFatal) {
        m_fatalError = fatalMsg;
        abort();  // 触发 SIGABRT，生成 tombstone 供后续分析
    }
}

void JZLogManager::logFmt(int module_id, int type, const char *format, ...)
{
    va_list var;
    va_start(var, format);
    std::string message = str_vasprintf(format, var);
    va_end(var);

    return log(module_id, type, message);
}

std::string JZLogManager::fatalError()
{
    return m_fatalError;
}