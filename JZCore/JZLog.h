#ifndef JZ_LOG_H_
#define JZ_LOG_H_
#include <atomic>
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
    LogModule_Agent,        // agent_cli 每轮 LLM 回合诊断(think/reply/tool_call/...), 单独模块
    LogModule_User = 100,
};

class JZFRAMEWORK_API JZLogModuleConfig
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

class JZFRAMEWORK_API JZLogItem
{
public:
    static std::shared_ptr<JZLogItem> makeLog(int module, int level, std::string message);

    int module;
    int level;
    int64_t time;
    std::string message;
};
typedef std::shared_ptr<JZLogItem> JZLogItemPtr;

class JZFRAMEWORK_API JZLogEvent : public JZEvent
{
public:
    JZLogEvent();
    JZLogItemPtr log;
};

class JZFRAMEWORK_API JZLogConfig
{
public:
    bool shouldPrintTime = false;      // 是否打印时间（最常用）
    bool shouldPrintToConsole = true;  // 是否输出到控制台
    bool shouldSaveToFile = false;     // 是否保存日志到文件
    bool shouldAutoSplitFile = false;  // 未实现(历史字段); 按天分割请用 rotateDaily
    std::string logPath;               // 日志保存**目录**

    // 按天轮转: 每天一个新文件 "<logPath>/<模块名>-<yyyyMMdd>.log"。
    // 关掉(默认)时固定写 "<logPath>/<模块名>.log" —— 老行为, 长驻进程会一直涨。
    // 何时切: 下一次 flush 发现日期变了就 close + 用当天文件名重开, 不用重启进程。
    bool rotateDaily = false;

    // 文件日志的落盘间隔(毫秒)。日志先攒在内存里, 由 JZLogManager 的定时器定期
    // flush —— 没有它, 就只剩析构时那一次 flush: 长驻进程(agent_node 这种)等于
    // "退出才看得到日志", 而且攒的那份会一直涨。<=0 表示不自动落盘。
    int flushIntervalMs = 1000;
};

class JZObject;
class JZTimer;

// 把配置里的 logFile 值解析成**日志目录** —— server / agent_cli / agent_node 三个宿主
// 用同一套口径, 免得一个把值当目录、另一个当完整文件路径(实测就有这种不一致):
//   ""                 -> defaultDir
//   "logs"             -> "logs"      (最后一段没有扩展名: 值本身就是目录)
//   "logs/"            -> "logs"
//   "logs/agent.log"   -> "logs"      (老配置写了文件名: 只取目录, 文件名固定由模块名决定)
//   "/var/log"         -> "/var/log"
//   "./logs"           -> "./logs"
JZFRAMEWORK_API std::string jzLogDirOf(const std::string &logFile, const std::string &defaultDir);

class JZFRAMEWORK_API JZLogManager
{
public:
    static JZLogManager *instance();
    
    void addObserver(int module, JZObject *recv);
    void removeObserver(int module, JZObject *recv);

    JZLogConfig config();
    void setConfig(JZLogConfig path);

    // 全局最小输出等级(LOG_DEBUG..LOG_FATAL): 低于它的日志直接丢弃, 不进控制台/文件/观察者。
    // 初值由环境变量决定, 进程启动时(JZLogManager 首次构造)只读一次:
    //     JZ_LOG_LEVEL = 0|1|2|3|4         (0=debug, 1=info, 2=warn, 3=error, 4=fatal)
    //     或 debug|info|warn|warning|error|fatal (不区分大小写)
    //     未设置/认不出来 = debug, 即不过滤, 与旧行为一致。
    // 运行中想改就调 setLogLevel()。
    // ⚠ LOG_FATAL 不受该等级控制, 任何配置下都输出(崩溃前的最后一条)。
    int logLevel();
    void setLogLevel(int level);

    void addModule(int module, JZLogModuleConfig config);
    std::string moduleName(int module);

    JZLogModuleConfig moduleConfig(int id);
    void setModuleConfig(int id, JZLogModuleConfig config);

    void log(int module, int level, const std::string &format);
    void logFmt(int module, int level, const char *format, ...);

    std::string fatalError();

protected:
    class JZFRAMEWORK_API JZLogModule
    {
    public:
        JZLogModuleConfig config;        
        JZFile file;
        // 当前打开的文件属于哪一天("yyyyMMdd"); 空 = 没开轮转。见 rotateDaily。
        std::string openDay;
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
    std::atomic<int> m_logLevel;    // 见 logLevel()/setLogLevel(); 热路径无锁读
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