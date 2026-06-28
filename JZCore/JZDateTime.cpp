#include <time.h>
#include <string.h>
#include <chrono>
#include "JZDateTime.h"

static tm toTm(int64_t msecs)
{
    time_t sec = static_cast<time_t>(msecs / 1000);
    tm t;
    memset(&t, 0, sizeof(tm));
    
#if defined(_WIN32)
    localtime_s(&t, &sec);
#else
    localtime_r(&sec, &t);
#endif
    return t;
}

static int64_t tmToMSecs(tm& t)
{
    time_t sec = mktime(&t);
    return static_cast<int64_t>(sec) * 1000;
}

JZDateTime JZDateTime::fromMSecsSinceEpoch(int64_t time)
{
    JZDateTime date;
    date.m_time = time;
    return date;
}

JZDateTime JZDateTime::currentDateTime()
{
    JZDateTime date;
    date.m_time = currentMSecsSinceEpoch();
    return date;
}

int64_t JZDateTime::currentMSecsSinceEpoch()
{
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    return ms;
}

JZDateTime::JZDateTime()
{
    m_time = 0;
}

JZDateTime::JZDateTime(int year, int month, int day, int hour, int minute, int sec)
{
    tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_sec = sec;
    m_time = tmToMSecs(t);
}

int64_t JZDateTime::toMSecsSinceEpoch() const
{
    return m_time;
}

void JZDateTime::setMSecsSinceEpoch(int64_t msecs)
{
    m_time = msecs;
}

int JZDateTime::year() const
{
    return toTm(m_time).tm_year + 1900;
}
int JZDateTime::month() const
{
    return toTm(m_time).tm_mon + 1;
}
int JZDateTime::day() const
{
    return toTm(m_time).tm_mday;
}
int JZDateTime::hour() const
{
    return toTm(m_time).tm_hour;
}
int JZDateTime::minute() const
{
    return toTm(m_time).tm_min;
}
int JZDateTime::second() const
{
    return toTm(m_time).tm_sec;
}

// ===================== 你要的 6个 SET 接口 =====================
void JZDateTime::setYear(int year)
{
    int ms = static_cast<int>(m_time % 1000);

    tm t = toTm(m_time);
    t.tm_year = year - 1900;
    m_time = tmToMSecs(t) + ms;
}

void JZDateTime::setMonth(int month)
{
    int ms = static_cast<int>(m_time % 1000);

    tm t = toTm(m_time);
    t.tm_mon = month - 1;
    m_time = tmToMSecs(t) + ms;
}

void JZDateTime::setDay(int day)
{
    int ms = static_cast<int>(m_time % 1000);

    tm t = toTm(m_time);
    t.tm_mday = day;
    m_time = tmToMSecs(t) + ms;
}

void JZDateTime::setHour(int hour)
{
    int ms = static_cast<int>(m_time % 1000);

    tm t = toTm(m_time);
    t.tm_hour = hour;
    m_time = tmToMSecs(t) + ms;
}

void JZDateTime::setMinute(int minute)
{
    int ms = static_cast<int>(m_time % 1000);

    tm t = toTm(m_time);
    t.tm_min = minute;
    m_time = tmToMSecs(t) + ms;
}

void JZDateTime::setSecond(int second)
{
    int ms = static_cast<int>(m_time % 1000);

    tm t = toTm(m_time);
    t.tm_sec = second;
    m_time = tmToMSecs(t) + ms;
}

// ===================== 你要的 addYears =====================
JZDateTime JZDateTime::addYears(int years) const
{
    int ms = static_cast<int>(m_time % 1000);

    tm t = toTm(m_time);
    t.tm_year += years;

    JZDateTime dt;
    dt.m_time = tmToMSecs(t);  // mktime 自动归一化日期
    dt.m_time += ms;           // 保留毫秒
    return dt;
}

// ===================== 统一风格：addMonths =====================
JZDateTime JZDateTime::addMonths(int months) const
{
    int ms = static_cast<int>(m_time % 1000);

    tm t = toTm(m_time);
    t.tm_mon += months;

    JZDateTime dt;
    dt.m_time = tmToMSecs(t);
    dt.m_time += ms;
    return dt;
}

// ===================== 统一风格：addDays =====================
JZDateTime JZDateTime::addDays(int days) const
{
    int ms = static_cast<int>(m_time % 1000);

    tm t = toTm(m_time);
    t.tm_mday += days;

    JZDateTime dt;
    dt.m_time = tmToMSecs(t);  
    dt.m_time += ms;
    return dt;
}

// ===================== 统一风格：addHours =====================
JZDateTime JZDateTime::addHours(int hours) const
{
    int ms = static_cast<int>(m_time % 1000);

    tm t = toTm(m_time);
    t.tm_hour += hours;

    JZDateTime dt;
    dt.m_time = tmToMSecs(t);
    dt.m_time += ms;
    return dt;
}

JZDateTime JZDateTime::addMinutes(int minutes) const
{
    int ms = static_cast<int>(m_time % 1000);

    tm t = toTm(m_time);
    t.tm_min += minutes;

    JZDateTime dt;
    dt.m_time = tmToMSecs(t);
    dt.m_time += ms;
    return dt;
}

JZDateTime JZDateTime::addSeconds(int seconds) const
{
    int ms = static_cast<int>(m_time % 1000);

    tm t = toTm(m_time);
    t.tm_sec += seconds;

    JZDateTime dt;
    dt.m_time = tmToMSecs(t);
    dt.m_time += ms;
    return dt;
}

int JZDateTime::weekDay() const
{
    return toTm(m_time).tm_wday;
}

int64_t JZDateTime::msecTo(JZDateTime& other) const
{
    return other.toMSecsSinceEpoch() - m_time;
}

std::string JZDateTime::toString(const std::string& format) const
{    
    int ms = static_cast<int>(m_time % 1000);
    tm tm = toTm(m_time);

    std::string result;
    result.reserve(format.size() + 16);

    for (size_t i = 0; i < format.size();)
    {
        if (i + 3 < format.size() && format[i] == 'y' && format[i + 1] == 'y' 
            && format[i + 2] == 'y' && format[i + 3] == 'y')
        {
            result += std::to_string(tm.tm_year + 1900);
            i += 4;
        }
        else if (i + 1 < format.size() && format[i] == 'M' && format[i + 1] == 'M')
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%02d", tm.tm_mon + 1);
            result += buf;
            i += 2;
        }
        else if (i + 1 < format.size() && format[i] == 'd' && format[i + 1] == 'd')
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%02d", tm.tm_mday);
            result += buf;
            i += 2;
        }
        else if ( (i + 1 < format.size() && format[i] == 'H' && format[i + 1] == 'H')
            || (i + 1 < format.size() && format[i] == 'h' && format[i + 1] == 'h'))
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%02d", tm.tm_hour);
            result += buf;
            i += 2;
        }
        else if (i + 1 < format.size() && format[i] == 'm' && format[i + 1] == 'm')
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%02d", tm.tm_min);
            result += buf;
            i += 2;
        }
        else if (i + 1 < format.size() && format[i] == 's' && format[i + 1] == 's')
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%02d", tm.tm_sec);
            result += buf;
            i += 2;
        }
        else if (i + 2 < format.size() && format[i] == 'z' && format[i + 1] == 'z' && format[i + 2] == 'z')
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%03d", ms);
            result += buf;
            i += 3;
        }
        else
        {
            result += format[i];
            ++i;
        }
    }

    return result;
}