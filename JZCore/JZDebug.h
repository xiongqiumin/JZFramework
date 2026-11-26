#ifndef JZ_DEBUG_H_
#define JZ_DEBUG_H_
#include <inttypes.h>
#include <string>
#include <vector>
#include <memory>
#include "JZLog.h"

// JZDebug: 流式日志门面(jzDebug() / jzInfo() / jzWarning() / jzError()).
//
// 输出等级统一由 JZLogManager 管(JZLogManager::logLevel()/setLogLevel()):
//     环境变量 JZ_LOG_LEVEL = 0|1|2|3|4 (0=debug, 1=info, 2=warn, 3=error, 4=fatal)
//     或 debug|info|warn|warning|error|fatal (不区分大小写), 进程启动时读一次。
// 低于该等级的语句在这里就早退(见 append()), 连字符串都不拼; JZLogManager::log() 里还有同样的兜底。
// 未设置/无法识别 = debug, 即不过滤, 与旧行为一致。LOG_FATAL 不受等级控制, 一定输出。
class JZFRAMEWORK_API JZDebug
{
public:
    explicit JZDebug(int level);
    ~JZDebug();

    JZDebug nospace();
    JZDebug space();
    bool isSpace() const;
    void setSpace(bool flag);

    JZDebug noquote();
    JZDebug quote();
    bool isQuote(bool flag) const;
    void setQuote(bool flag);
    
    JZDebug &operator<<(bool i);
    JZDebug &operator<<(int i);
    JZDebug &operator<<(unsigned int u32);
    JZDebug &operator<<(long i64);
    JZDebug &operator<<(unsigned long u64);
    JZDebug &operator<<(long long i64);
    JZDebug &operator<<(unsigned long long u64);
    JZDebug &operator<<(double i);
    JZDebug &operator<<(float f);
    JZDebug &operator<<(void *);
    JZDebug &operator<<(JZ::EndFlag endl);

    JZDebug &operator<<(const char *str);
    JZDebug &operator<<(const std::string &str);

    void append(const std::string& msg);

protected:    
    struct JZFRAMEWORK_API JZDebugData{
        JZDebugData();
        ~JZDebugData();
        
        int logLevel;
        bool isSpace;
        bool isQuote;
        bool enabled;       // 等级低于 JZLogManager::logLevel() 时为 false: 不拼串也不输出
        std::string  message;
    };

    std::shared_ptr<JZDebugData> d;   
};
// ⚠ 这几个是**自由函数**: 动态库模式下必须带 JZFRAMEWORK_API(dllexport/dllimport),
//   否则 exe 侧链接时报 LNK2019 "无法解析的外部符号 jzInfo"(它们没有类的导出属性可借)。
JZFRAMEWORK_API JZDebug jzDebug();
JZFRAMEWORK_API JZDebug jzInfo();
JZFRAMEWORK_API JZDebug jzWarning();
JZFRAMEWORK_API JZDebug jzError();
JZFRAMEWORK_API void jzDebug(const char *format, ...);
JZFRAMEWORK_API void jzInfo(const char *format, ...);
JZFRAMEWORK_API void jzWarning(const char *format, ...);
JZFRAMEWORK_API void jzError(const char *format, ...);

template<class T>
JZDebug operator<<(JZDebug d, const std::vector<T>& list)
{
    d << "[";
    bool old_flag = d.isSpace();
    d.setSpace(false);
    for (size_t i = 0; i < list.size(); i++)
    {
        d << list[i];
        if (i != list.size() - 1)
            d << ", ";
    }
    d << "]";
    d.setSpace(old_flag);
    return d;
}

template<class KEY,class VALUE>
JZDebug operator<<(JZDebug d, const std::map<KEY,VALUE>& map)
{
    d << "{";
    bool old_flag = d.isSpace();
    d.setSpace(false);
    int index = 0;
    auto it = map.begin();
    while(it != map.end())
    {
        d << it->first << ":" << it->second;
        if (index != map.size() - 1)
            d << ", ";

        it++;
        index++;    
    }
    d << "}";
    d.setSpace(old_flag);
    return d;
}

// 拉入与 JZOptional / JZResult 的流式输出算子 (定义见 detail/JZDebugDetail.h,
// 仅当对应类型也可见时生成, 两者之间零 #include 耦合).
#include "detail/JZDebugDetail.h"

#endif // !JZ_DEBUG_H_
