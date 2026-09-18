#ifndef JZ_LOG_H_
#define JZ_LOG_H_

#include <memory>
#include <vector>
#include <string>
#include "JZGlobal.h"
#include "JZFile.h"
#include "JZEvent.h"
#include "JZMutex.h"

enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR,
    LOG_FATAL,
};

enum {
    LogModule_General,
    LogModule_User = 100,
};

class JZLogModuleConfig
{
public:
    enum {
        None,
        File = 0x1,
        Console = 0x2,
    };

    JZLogModuleConfig();

    std::string name;
    int type;
};

class JZLogItem
{
public:
    static std::shared_ptr<JZLogItem> makeLog(int module, int level, std::string message);

    int module;
    int level;
    int64_t time;
    std::string message;
};
typedef std::shared_ptr<JZLogItem> JZLogItemPtr;

class JZLogEvent : public JZEvent
{
public:
    JZLogEvent();
    JZLogItemPtr log;
};

class JZLogConfig
{
public:
    bool shouldPrintTime = false;      // 是否打印时间（最常用）
    bool shouldPrintToConsole = true;  // 是否输出到控制台
    bool shouldSaveToFile = false;     // 是否保存日志到文件
    bool shouldAutoSplitFile = false;  // 是否自动分割文件
    std::string logPath;               // 日志保存路径

    // 文件日志的落盘间隔(毫秒)。日志先攒在内存里, 由 JZLogManager 的定时器定期
    // flush —— 没有它, 就只剩析构时那一次 flush: 长驻进程(agent_node 这种)等于
    // "退出才看得到日志", 而且攒的那份会一直涨。<=0 表示不自动落盘。
    int flushIntervalMs = 1000;
};

class JZObject;
class JZTimer;

class JZLogManager
{
public:
    static JZLogManager *instance();
    
    void addObserver(int module, JZObject *recv);
    void removeObserver(int module, JZObject *recv);

    JZLogConfig config();
    void setConfig(JZLogConfig path);

    void addModule(int module, JZLogModuleConfig config);
    std::string moduleName(int module);

    JZLogModuleConfig moduleConfig(int id);
    void setModuleConfig(int id, JZLogModuleConfig config);

    void log(int module, int level, const std::string &format);
    void logFmt(int module, int level, const char *format, ...);

    std::string fatalError();

protected:
    class JZLogModule
    {
    public:
        JZLogModuleConfig config;        
        JZFile file;
        std::vector<JZObject*> recvObjects;
        std::vector<JZLogItemPtr> logCache;
    };

    JZLogManager();
    ~JZLogManager();
    const char* getLevelPrefix(int level);

    void flushFile();
    // 按当前配置起/停落盘定时器: 有模块开了 File 且间隔 > 0 才跑。
    // ⚠ 需要 jzApp(JZTimer 构造要取 jzApp->currentThread()), 没有 app 时什么都不做 ——
    //   先记着配置, 等下次 setConfig / setModuleConfig 时再试。
    void updateFlushTimer();

    std::map<int, JZLogModule*> m_modules;
    JZLogConfig m_config;
    JZMutex m_lock;
    std::string m_fatalError;
    JZTimer *m_flushTimer = nullptr;
};

#define LOG_D(message) JZLogManager::instance()->log(LogModule_General,LOG_DEBUG,message)
#define LOG_I(message) JZLogManager::instance()->log(LogModule_General,LOG_INFO,message)
#define LOG_W(message) JZLogManager::instance()->log(LogModule_General,LOG_WARNING,message)
#define LOG_E(message) JZLogManager::instance()->log(LogModule_General,LOG_ERROR,message)
#define LOG_F(message) JZLogManager::instance()->log(LogModule_General,LOG_FATAL,message)

#define LOGMOD_D(module,format) JZLogManager::instance()->log(module,LOG_DEBUG,format)
#define LOGMOD_I(module,format) JZLogManager::instance()->log(module,LOG_INFO,format)
#define LOGMOD_W(module,format) JZLogManager::instance()->log(module,LOG_WARNING,format)
#define LOGMOD_E(module,format) JZLogManager::instance()->log(module,LOG_ERROR,format)
#define LOGMOD_F(module,format) JZLogManager::instance()->log(module,LOG_FATAL,format)

#define LOG_FMT_D(format,...) JZLogManager::instance()->logFmt(LogModule_General,LOG_DEBUG,format,##__VA_ARGS__)
#define LOG_FMT_I(format,...) JZLogManager::instance()->logFmt(LogModule_General,LOG_INFO,format,##__VA_ARGS__)
#define LOG_FMT_W(format,...) JZLogManager::instance()->logFmt(LogModule_General,LOG_WARNING,format,##__VA_ARGS__)
#define LOG_FMT_E(format,...) JZLogManager::instance()->logFmt(LogModule_General,LOG_ERROR,format,##__VA_ARGS__)
#define LOG_FMT_F(format,...) JZLogManager::instance()->logFmt(LogModule_General,LOG_FATAL,format,##__VA_ARGS__)

#define LOGMOD_FMT_D(module,message,...) JZLogManager::instance()->logFmt(module,LOG_DEBUG,message,##__VA_ARGS__)
#define LOGMOD_FMT_I(module,message,...) JZLogManager::instance()->logFmt(module,LOG_INFO,message,##__VA_ARGS__)
#define LOGMOD_FMT_W(module,message,...) JZLogManager::instance()->logFmt(module,LOG_WARNING,message,##__VA_ARGS__)
#define LOGMOD_FMT_E(module,message,...) JZLogManager::instance()->logFmt(module,LOG_ERROR,message,##__VA_ARGS__)
#define LOGMOD_FMT_F(module,message,...) JZLogManager::instance()->logFmt(module,LOG_FATAL,message,##__VA_ARGS__)

#endif