#ifndef JZ_DATE_TIME_H_
#define JZ_DATE_TIME_H_
#include <inttypes.h>
#include <string>
#include "JZGlobal.h"

class JZFRAMEWORK_API JZDateTime
{
public:    
    static int64_t currentMSecsSinceEpoch();
    static JZDateTime currentDateTime();
    static JZDateTime fromMSecsSinceEpoch(int64_t time);

    JZDateTime();
    JZDateTime(int year, int month, int day,int hour = 0,int minute = 0,int sec = 0);

    int64_t toMSecsSinceEpoch() const;
    void setMSecsSinceEpoch(int64_t msecs);

    int64_t msecTo(JZDateTime &other) const;
    
    int year() const;
    int month() const;
    int day() const;

    int hour() const;
    int minute() const;
    int second() const;

    void setYear(int year);
    void setMonth(int month);
    void setDay(int day);
    void setHour(int hour);
    void setMinute(int minute);
    void setSecond(int second);

    JZDateTime addYears(int years) const;
    JZDateTime addMonths(int months) const;
    JZDateTime addDays(int days) const;
    JZDateTime addHours(int hours) const;
    JZDateTime addMinutes(int minutes) const;
    JZDateTime addSeconds(int seconds) const;

    int weekDay() const;

    std::string toString(const std::string& format) const;

protected:
    int64_t m_time;
};

#endif