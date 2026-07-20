#ifndef JZ_DEBUG_H_
#define JZ_DEBUG_H_

#include <inttypes.h>
#include <string>
#include <vector>
#include <memory>
#include "JZLog.h"

class JZDebug
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
    struct JZDebugData{
        JZDebugData();
        ~JZDebugData();
        
        int logLevel;
        bool isSpace;
        bool isQuote;
        std::string  message;
    };

    std::shared_ptr<JZDebugData> d;   
};
JZDebug jzDebug();
JZDebug jzInfo();
JZDebug jzWarning();
JZDebug jzError();
void jzDebug(const char *format, ...);
void jzInfo(const char *format, ...);
void jzWarning(const char *format, ...);
void jzError(const char *format, ...);

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

// 拉入与 JZOptional / JZResult 的流式输出算子 (定义见 detail/JZDebugDetail.h,
// 仅当对应类型也可见时生成, 两者之间零 #include 耦合).
#include "detail/JZDebugDetail.h"

#endif // !JZ_DEBUG_H_
