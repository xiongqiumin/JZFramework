#ifndef JZ_ELAPSED_TIMER_H_
#define JZ_ELAPSED_TIMER_H_
#include "JZGlobal.h"
#include <inttypes.h>

class JZFRAMEWORK_API JZElapsedTimer {
public:    
    JZElapsedTimer();
    ~JZElapsedTimer();
    
    void start();
    int64_t restart();
    void invalidate();
    bool isValid() const;
    
    int64_t elapsed() const;
    bool hasExpired(int64_t timeout) const;
    int64_t nsecsElapsed() const;

private:
    int64_t currentTimeNs() const; 

    bool m_valid;       
    int64_t m_startNs;      
};

#endif