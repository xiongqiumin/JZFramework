#include <assert.h>
#include <stdarg.h>
#include "JZLog.h"
#include "JZDateTime.h"
#include "JZApplication.h"
#include "JZTimer.h"
#include "JZStd.h"      // str_utf8_to_console(): 控制台输出编码, 见下面 toConsoleBytes
#include "JZPlatform.h" // JZPlatform::getEnv(): 环境变量统一从这里取

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

// ==================== 日志目录解析(三个宿主共用) ====================
std::string jzLogDirOf(const std::string &logFile, const std::string &defaultDir)
{
    auto isSep = [](char c) { return c == '/' || c == '\\'; };

    std::string v = logFile;
    // 末尾分隔符没有意义, 去掉(全是分隔符的极端情况保留一个)
    while (v.size() > 1 && isSep(v.back()))
        v.pop_back();
    if (v.empty())
        return defaultDir;
    if (v == "/" || v == "\\")
        return v;

    // 最后一段像"文件名"(带扩展名)时只取目录 —— 兼容老配置把完整文件路径写进来
    size_t sep = v.find_last_of("/\\");
    std::string last = (sep == std::string::npos) ? v : v.substr(sep + 1);
    if (last.find('.') != std::string::npos)
    {
        if (sep == std::string::npos)
            return ".";                 // "agent.log" -> 当前目录
        std::string dir = v.substr(0, sep);
        return dir.empty() ? std::string("/") : dir;
    }
    return v;
}

// 当天标记("yyyyMMdd"), 作按天轮转的文件名后缀
static std::string dailyStamp()
{
    return JZDateTime::currentDateTime().toString("yyyyMMdd");
}

// "<logPath>/<模块名>[-<yyyyMMdd>].log"; logPath 为空时落当前目录
static std::string logFilePathOf(const JZLogConfig &cfg, const std::string &moduleName,
                                 const std::string &day)
{
    std::string path = cfg.logPath;
    if (!path.empty() && path.back() != '/' && path.back() != '\\')
        path += "/";
    path += moduleName;
    if (!day.empty())
        path += "-" + day;
    path += ".log";
    return path;
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

// 环境变量 JZ_LOG_LEVEL 指定的最小输出等级(不区分大小写, 允许首尾空格):
//   数字 0/1/2/3/4 = debug/info/warn/error/fatal
//   或名字 debug|info|warn|warning|error|fatal
//   没设置/认不出来 -> LOG_DEBUG(不过滤, 保持旧行为)。
// ⚠ LOG_FATAL 不参与过滤, 见 log()。
static bool parseLogLevel(const std::string &v, int *level)
{
    if (v == "debug" || v == "d")   { *level = LOG_DEBUG;   return true; }
    if (v == "info" || v == "i")    { *level = LOG_INFO;    return true; }
    if (v == "warn" || v == "warning" || v == "w") { *level = LOG_WARNING; return true; }
    if (v == "error" || v == "err" || v == "e")    { *level = LOG_ERROR;   return true; }
    if (v == "fatal" || v == "f")   { *level = LOG_FATAL;   return true; }

    bool ok = false;
    int n = str_toInt(v, &ok);
    if (ok && n >= LOG_DEBUG && n <= LOG_FATAL)
    {
        *level = n;
        return true;
    }
    return false;
}

static int logLevelFromEnv()
{
    std::string v = str_trimmed(str_toLower(JZPlatform::getEnv("JZ_LOG_LEVEL")));
    int level = LOG_DEBUG;
    if (!v.empty() && parseLogLevel(v, &level))
        return level;

    return LOG_DEBUG;
}

JZLogManager *JZLogManager::instance()
{
    static JZLogManager inst;
    return &inst;
}

JZLogManager::JZLogManager()
{
    g_logEnabled = true;
    m_logLevel.store(logLevelFromEnv(), std::memory_order_relaxed);
    
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

int JZLogManager::logLevel()
{
    return m_logLevel.load(std::memory_order_relaxed);
}

void JZLogManager::setLogLevel(int level)
{
    if (level < LOG_DEBUG || level > LOG_FATAL)
        return;

    m_logLevel.store(level, std::memory_order_relaxed);
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
        // 文件名由模块名决定(模块名就是配置里给的名字, 如 Server / Agent / Node)
        m_modules[id]->openDay = m_config.rotateDaily ? dailyStamp() : std::string();
        m_modules[id]->file.open(logFilePathOf(m_config, config.name, m_modules[id]->openDay), "wt");
    }
    else
    {
        m_modules[id]->file.close();
        m_modules[id]->openDay.clear();
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

    // 按天轮转: 日期一变就把文件换成当天的(不重启进程也能切)
    const std::string today = m_config.rotateDaily ? dailyStamp() : std::string();

    auto it = m_modules.begin();
    while (it != m_modules.end())
    {
        auto &logs = it->second->logCache;
        if (m_config.rotateDaily && it->second->file.isOpen() && it->second->openDay != today)
        {
            it->second->file.close();
            it->second->openDay = today;
            it->second->file.open(logFilePathOf(m_config, it->second->config.name, today), "wt");
        }
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
//
// ⚠ 这套规则已经提到 JZCore/JZStd.cpp 的 str_utf8_to_console()(声明在 JZStd.h), 这里直接调用:
//   示例/test 自己往控制台 printf 中文时用同一个函数即可, 不要再抄一份。
static std::string toConsoleBytes(const std::string &utf8)
{
    return str_utf8_to_console(utf8);
}

void JZLogManager::log(int module_id, int level, const std::string &message)
{
    // 等级过滤放最前面: 热路径上只多一次无锁原子读, 不合格的连锁都不用进。
    // JZDebug 侧已做了同样的早退(避免拼串), 这里是兜底 —— LOG_D/LOG_I 等宏、
    // logFmt、各模块直接的 log() 调用全走这一条。
    // LOG_FATAL 无条件放行(它是崩溃前的最后一条, 不能被等级吞掉)。
    if (!g_logEnabled)
        return;
    if (level != LOG_FATAL && level < m_logLevel.load(std::memory_order_relaxed))
        return;

    JZMutexLocker locker(&m_lock);
    if (!map_contains(m_modules,module_id))
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