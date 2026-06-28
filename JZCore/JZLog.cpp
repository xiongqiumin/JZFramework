#include <assert.h>
#include <stdarg.h>
#include "JZLog.h"
#include "JZDateTime.h"
#include "JZApplication.h"

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

void JZLogManager::flushFile()
{
    auto it = m_modules.begin();
    while (it != m_modules.end())
    {
        auto &logs = it->second->logCache;
        for (int line = 0; line < (int)logs.size(); line++)
        {
            auto log = logs[line].get();

            JZDateTime time = JZDateTime::fromMSecsSinceEpoch(log->time);
            std::string log_save = time.toString("[yyyy-MM-dd HH:mm:ss zzz] ");
            log_save += log->message + "\n";

            it->second->file.write(log_save.c_str(), (int)log_save.size());
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

#if WIN32
    printf("%s\n", str_utf8_to_local(message).c_str());
#else
    printf("%s\n", message.c_str());
#endif

#endif
    }

    for (int i = (int)module->recvObjects.size() - 1; i >= 0; i--)
    {
        JZLogEvent *event = new JZLogEvent();
        event->log = log_ptr;
        jzApp->postEvent(module->recvObjects[i], event);
    }

    if (level == LOG_FATAL) {
        m_fatalError = message;
        locker.unlock();
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