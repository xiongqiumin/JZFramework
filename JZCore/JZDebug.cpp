#include <stdarg.h>
#include "JZDebug.h"
#include "JZLog.h"
#include "JZStd.h"

using namespace std;

JZDebug::JZDebugData::JZDebugData()
{
    isSpace = true;
    isQuote = true;
    logLevel = 0;
}

JZDebug::JZDebugData::~JZDebugData()
{
    JZLogManager::instance()->log(LogModule_General,logLevel,message);
}

JZDebug::JZDebug(int level)
{
    d = std::make_shared<JZDebugData>();
    d->logLevel = level;
}

JZDebug::~JZDebug()
{

}

JZDebug JZDebug::nospace()
{
    d->isSpace = false;
    return *this;
}

JZDebug JZDebug::space()
{
    d->isSpace = true;
    return *this;
}

bool JZDebug::isSpace() const
{
    return d->isSpace;
}

void JZDebug::setSpace(bool flag)
{
    d->isSpace = flag;
}

JZDebug JZDebug::quote() 
{
    d->isQuote = true;
    return *this;
}

JZDebug JZDebug::noquote() 
{
    d->isQuote = false;
    return *this;
}

bool JZDebug::isQuote(bool flag) const
{
    return d->isQuote;
}

void JZDebug::setQuote(bool flag)
{
    d->isQuote = flag;
}

void JZDebug::append(const string &msg)
{
    if (d->isSpace)
    {
        if (!d->message.empty())
            d->message += " ";
    }
    d->message += msg;
}

JZDebug &JZDebug::operator<<(bool b)
{
    append(b? "true":"false");
    return *this;
}

JZDebug &JZDebug::operator<<(int i)
{
    append(to_string(i));
    return *this;
}

JZDebug& JZDebug::operator<<(unsigned int u32)
{
    append(to_string(u32));
    return *this;
}

JZDebug &JZDebug::operator<<(long l)
{
    append(to_string(l));
    return *this;
}

JZDebug& JZDebug::operator<<(unsigned long ul)
{
    append(to_string(ul));
    return *this;
}

JZDebug &JZDebug::operator<<(long long i64)
{
    append(to_string(i64));
    return *this;
}

JZDebug& JZDebug::operator<<(unsigned long long u64)
{
    append(to_string(u64));
    return *this;
}

JZDebug &JZDebug::operator<<(float f)
{
    append(to_string(f));
    return *this;
}

JZDebug &JZDebug::operator<<(double d)
{
    append(to_string(d));
    return *this;
}

JZDebug &JZDebug::operator<<(void *ptr)
{
    char buffer[64] = {};
    snprintf(buffer,64,"%p",ptr);

    append(buffer);
    return *this;
}

JZDebug &JZDebug::operator<<(JZ::EndFlag endl)
{
    append("\n");
    return *this; 
}

JZDebug &JZDebug::operator<<(const char *str)
{    
    append(str);
    return *this;
}

JZDebug &JZDebug::operator<<(const string &str)
{
    if(d->isQuote)
    {
        string tmp = "\"" + str_escape(str) + "\"";
        append(tmp);
    }
    else
    {
        append(str);
    }

    return *this;
}

JZDebug jzDebug()
{
    return JZDebug(LOG_DEBUG);
}

JZDebug jzInfo()
{
    return JZDebug(LOG_INFO);
}

JZDebug jzWarning()
{
    return JZDebug(LOG_WARNING);
}

JZDebug jzError()
{
    return JZDebug(LOG_ERROR);
}

static string vformat(const char *fmt, va_list ap)
{
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int bufSize = vsnprintf(nullptr, 0, fmt, ap_copy);
    va_end(ap_copy);

    if (bufSize <= 0)
        return "";

    vector<char> buf(bufSize + 1, 0);
    vsnprintf(buf.data(), buf.size(), fmt, ap);
    return string(buf.data());
}

void jzDebug(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    string msg = vformat(format, ap);
    va_end(ap);
    jzDebug().noquote() << msg;
}

void jzInfo(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    string msg = vformat(format, ap);
    va_end(ap);
    jzInfo().noquote() << msg;
}

void jzWarning(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    string msg = vformat(format, ap);
    va_end(ap);
    jzWarning().noquote() << msg;
}

void jzError(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    string msg = vformat(format, ap);
    va_end(ap);
    jzError().noquote() << msg;
}